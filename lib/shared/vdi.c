/*
 * Copyright (C) 2015 China Mobile Inc.
 *
 * Liu Yuan <liuyuan@cmss.chinamobile.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "sheepdog.h"
#include "internal.h"

static int write_object(struct sd_cluster *c, uint64_t oid, uint64_t cow_oid,
	void *data, unsigned int datalen, uint64_t offset, uint32_t flags,
	bool create, bool direct);

static int read_object(struct sd_cluster *c, uint64_t oid, void *data,
		unsigned int datalen, uint64_t offset, bool direct);

static int vdi_read_inode(struct sd_cluster *c, char *name,
	char *tag, struct sd_inode *inode, bool onlyheader);

static int find_vdi(struct sd_cluster *c, char *name,
					char *tag, uint32_t *vid);

static int lock_vdi(struct sd_vdi *vdi)
{
	struct sd_req hdr = {};
	struct sd_rsp *rsp = (struct sd_rsp *)&hdr;
	int ret;

	hdr.opcode = SD_OP_LOCK_VDI;
	hdr.data_length = SD_MAX_VDI_LEN;
	hdr.flags = SD_FLAG_CMD_WRITE;
	ret = sd_run_sdreq(vdi->cluster, &hdr, vdi->name);
	if (ret != SD_RES_SUCCESS)
		return ret;

	vdi->vid = rsp->vdi.vdi_id;

	return SD_RES_SUCCESS;
}

static int unlock_vdi(struct sd_vdi *vdi)
{
	struct sd_req hdr = {};
	int ret;

	hdr.opcode = SD_OP_RELEASE_VDI;
	hdr.vdi.type = LOCK_TYPE_NORMAL;
	hdr.vdi.base_vdi_id = vdi->vid;
	ret = sd_run_sdreq(vdi->cluster, &hdr, NULL);
	if (ret != SD_RES_SUCCESS)
		return ret;

	return SD_RES_SUCCESS;
}

static struct sd_vdi *alloc_vdi(struct sd_cluster *c, char *name)
{
	struct sd_vdi *new = xzalloc(sizeof(*new));

	new->cluster = c;
	new->name = name;
	new->inode = xmalloc(sizeof(struct sd_inode));
	sd_init_rw_lock(&new->lock);

	return new;
}

static void free_vdi(struct sd_vdi *vdi)
{
	sd_destroy_rw_lock(&vdi->lock);
	free(vdi->inode);
	free(vdi);
}

struct sd_vdi *sd_vdi_open(struct sd_cluster *c, char *name)
{
	struct sd_req hdr = {};
	struct sd_vdi *new = alloc_vdi(c, name);
	int ret;

	ret = lock_vdi(new);
	if (ret != SD_RES_SUCCESS) {
		errno = ret;
		goto out_free;
	}

	hdr.opcode = SD_OP_READ_OBJ;
	hdr.data_length = SD_INODE_SIZE;
	hdr.obj.oid = vid_to_vdi_oid(new->vid);
	hdr.obj.offset = 0;
	ret = sd_run_sdreq(c, &hdr, new->inode);
	if (ret != SD_RES_SUCCESS) {
		errno = ret;
		goto out_unlock;
	}

	if (vdi_is_snapshot(new->inode)) {
		errno = SD_RES_INVALID_PARMS;
		goto out_unlock;
	}

	return new;
out_unlock:
	unlock_vdi(new);
out_free:
	free_vdi(new);
	return NULL;
}

static void queue_request(struct sd_request *req)
{
	struct sd_cluster *c = req->vdi->cluster;

	sd_write_lock(&c->request_lock);
	list_add_tail(&req->list, &c->request_list);
	sd_rw_unlock(&c->request_lock);

	eventfd_xwrite(c->request_fd, 1);
}

static void free_request(struct sd_request *req)
{
	close(req->efd);
	free(req);
}

static struct sd_request *alloc_request(struct sd_vdi *vdi, void *buf,
			size_t count, off_t offset, bool iswrite)
{
	struct sd_request *req;
	int fd;

	fd = eventfd(0, 0);
	if (fd < 0) {
		errno = SD_RES_SYSTEM_ERROR;
		return NULL;
	}
	req = xzalloc(sizeof(*req));
	req->efd = fd;
	req->data = buf;
	req->length = count;
	req->offset = offset;
	req->write = iswrite;
	INIT_LIST_NODE(&req->list);
	req->vdi = vdi;

	return req;
}

int sd_vdi_read(struct sd_vdi *vdi, void *buf, size_t count, off_t offset)
{
	struct sd_request *req = alloc_request(vdi, buf, count, offset, false);
	int ret;

	if (!req)
		return errno;

	queue_request(req);

	eventfd_xread(req->efd);
	ret = req->ret;
	free_request(req);

	return ret;
}

int sd_vdi_write(struct sd_vdi *vdi, void *buf, size_t count, off_t offset)
{
	struct sd_request *req = alloc_request(vdi, buf, count, offset, true);
	int ret;

	if (!req)
		return errno;

	queue_request(req);

	eventfd_xread(req->efd);
	ret = req->ret;
	free_request(req);

	return ret;
}

int sd_vdi_close(struct sd_vdi *vdi)
{
	int ret;

	ret = unlock_vdi(vdi);
	if (ret != SD_RES_SUCCESS) {
		fprintf(stderr, "failed to unlock %s\n", vdi->name);
		return ret;
	}
	free_vdi(vdi);
	return 0;
}

static int do_vdi_create(struct sd_cluster *c, char *name, uint64_t vdi_size,
			uint32_t base_vid, uint32_t *vdi_id,
			bool snapshot, uint8_t store_policy)
{
	struct sd_req hdr = {};
	struct sd_rsp *rsp = (struct sd_rsp *)&hdr;
	int ret;

	hdr.opcode = SD_OP_NEW_VDI;
	hdr.flags = SD_FLAG_CMD_WRITE;
	hdr.data_length = SD_MAX_VDI_LEN;

	hdr.vdi.base_vdi_id = base_vid;
	hdr.vdi.snapid = snapshot ? 1 : 0;
	hdr.vdi.vdi_size = vdi_size;
	hdr.vdi.store_policy = store_policy;

	ret = sd_run_sdreq(c, &hdr, name);
	if (ret != SD_RES_SUCCESS)
		return ret;

	if (vdi_id)
		*vdi_id = rsp->vdi.vdi_id;

	return SD_RES_SUCCESS;
}

static int write_object(struct sd_cluster *c, uint64_t oid, uint64_t cow_oid,
	void *data, unsigned int datalen, uint64_t offset, uint32_t flags,
	bool create, bool direct)
{
	struct sd_req hdr = {};
	struct sd_rsp *rsp = (struct sd_rsp *)&hdr;
	int ret;

	if (create)
		hdr.opcode = SD_OP_CREATE_AND_WRITE_OBJ;
	else
		hdr.opcode = SD_OP_WRITE_OBJ;
	hdr.data_length = datalen;
	hdr.flags = flags | SD_FLAG_CMD_WRITE;
	if (cow_oid)
		hdr.flags |= SD_FLAG_CMD_COW;
	if (direct)
		hdr.flags |= SD_FLAG_CMD_DIRECT;

	hdr.obj.oid = oid;
	hdr.obj.cow_oid = cow_oid;
	hdr.obj.offset = offset;

	ret = sd_run_sdreq(c, &hdr, data);
	if (ret != SD_RES_SUCCESS)
		return ret;

	if (rsp->result != SD_RES_SUCCESS)
		return rsp->result;

	return SD_RES_SUCCESS;
}

static int read_object(struct sd_cluster *c, uint64_t oid, void *data,
		unsigned int datalen, uint64_t offset, bool direct)
{
	struct sd_req hdr = {};
	struct sd_rsp *rsp = (struct sd_rsp *)&hdr;
	int ret;

	hdr.opcode = SD_OP_READ_OBJ;
	hdr.data_length = datalen;
	hdr.obj.oid = oid;
	hdr.obj.offset = offset;
	if (direct)
		hdr.flags |= SD_FLAG_CMD_DIRECT;

	ret = sd_run_sdreq(c, &hdr, data);
	if (ret != SD_RES_SUCCESS)
		return ret;

	if (rsp->result != SD_RES_SUCCESS)
		return rsp->result;

	return SD_RES_SUCCESS;
}

static int find_vdi(struct sd_cluster *c, char *name,
					char *tag, uint32_t *vid)
{
	struct sd_req hdr = {};
	struct sd_rsp *rsp = (struct sd_rsp *)&hdr;
	char buf[SD_MAX_VDI_LEN + SD_MAX_VDI_TAG_LEN];
	int ret;

	memset(buf, 0, sizeof(buf));
	pstrcpy(buf, SD_MAX_VDI_LEN, name);
	if (tag)
		pstrcpy(buf + SD_MAX_VDI_LEN, SD_MAX_VDI_TAG_LEN, tag);

	hdr.opcode = SD_OP_GET_VDI_INFO;
	hdr.data_length = SD_MAX_VDI_LEN + SD_MAX_VDI_TAG_LEN;
	hdr.flags = SD_FLAG_CMD_WRITE;

	ret = sd_run_sdreq(c, &hdr, buf);
	if (ret != SD_RES_SUCCESS)
		return ret;

	if (rsp->result == SD_RES_SUCCESS && vid)
		*vid = rsp->vdi.vdi_id;

	return rsp->result;
}

static int vdi_read_inode(struct sd_cluster *c, char *name,
	char *tag, struct sd_inode *inode, bool onlyheader)
{
	int ret;
	uint32_t vid;
	size_t len;

	ret = find_vdi(c, name, tag, &vid);
	if (ret != SD_RES_SUCCESS)
		return ret;

	if (onlyheader)
		len = SD_INODE_HEADER_SIZE;
	else
		len = SD_INODE_SIZE;

	ret = read_object(c, vid_to_vdi_oid(vid), inode, len, 0, true);

	return ret;
}

int sd_vdi_snapshot(struct sd_cluster *c, char *name, char *snap_tag)
{
	char buf[SD_INODE_HEADER_SIZE];
	struct sd_inode *inode = (struct sd_inode *)buf;
	int ret = 0;

	if (!snap_tag || *snap_tag == '\0') {
		fprintf(stderr, "Snapshot tag can NOT be null!\n");
		return SD_RES_INVALID_PARMS;
	}

	if (!name || *name == '\0') {
		fprintf(stderr, "VDI name can NOT be null!\n");
		return SD_RES_INVALID_PARMS;
	}

	ret = find_vdi(c, name, snap_tag, NULL);

	if (ret == SD_RES_SUCCESS) {
		fprintf(stderr, "Failed to create snapshot: "
			"Tag %s is already existed\n", snap_tag);
		return SD_RES_INVALID_PARMS;

	} else if (ret == SD_RES_NO_TAG) {
		ret = vdi_read_inode(c, name, NULL, inode, true);
		if (ret != SD_RES_SUCCESS) {
			fprintf(stderr, "Failed to read inode: %s\n", name);
			return ret;
		}

	} else {
		fprintf(stderr, "Failed to create snapshot:%s\n",
				sd_strerr(ret));
		return ret;
	}

	if (inode->store_policy) {
		fprintf(stderr, "Creating a snapshot of hypervolume"
				" is not supported\n");
		return SD_RES_INVALID_PARMS;
	}

	ret = write_object(c, vid_to_vdi_oid(inode->vdi_id), 0, snap_tag,
			SD_MAX_VDI_TAG_LEN, offsetof(struct sd_inode, tag), 0,
			false, false);

	if (ret != SD_RES_SUCCESS) {
		fprintf(stderr, "Failed to write object:%"PRIx64"\n",
				vid_to_vdi_oid(inode->vdi_id));
		return ret;
	}

	ret = do_vdi_create(c, inode->name, inode->vdi_size,
			inode->vdi_id, NULL, true, 0);

	if (ret != SD_RES_SUCCESS)
		fprintf(stderr, "Failed to snapshot:%s\n", sd_strerr(ret));

	return ret;
}

int sd_vdi_create(struct sd_cluster *c, char *name, uint64_t size)
{
	int ret;

	if (size > SD_MAX_VDI_SIZE) {
		fprintf(stderr, "VDI size is too large\n");
		return SD_RES_INVALID_PARMS;
	}

	if (size == 0) {
		fprintf(stderr, "VDI size must be larger than 0\n");
		return SD_RES_INVALID_PARMS;
	}

	if (!name || *name == '\0') {
		fprintf(stderr, "VDI name can NOT be null!\n");
		return SD_RES_INVALID_PARMS;
	}

	uint8_t store_policy = 0;
	if (size > SD_OLD_MAX_VDI_SIZE)
		store_policy = 1;/** for hyper volume **/

	ret = do_vdi_create(c, name, size, 0, NULL,
			false, store_policy);

	return ret;
}

int sd_vdi_clone(struct sd_cluster *c, char *srcname,
		char *srctag, char *dstname)
{
	int ret;
	struct sd_inode *inode = NULL;

	if (!dstname || *dstname == '\0') {
		ret = SD_RES_INVALID_PARMS;
		fprintf(stderr, "Destination VDI name can NOT be null\n");
		goto out;
	}

	if (!srctag || *srctag == '\0') {
		ret = SD_RES_INVALID_PARMS;
		fprintf(stderr, "Only snapshot VDIs can be cloned, "
				"please specify snapshot tag\n");
		goto out;
	}

	if (!srcname || *srcname == '\0') {
		fprintf(stderr, "Source VDI name can NOT be null!\n");
		ret = SD_RES_INVALID_PARMS;
		goto out;
	}

	inode = xmalloc(sizeof(struct sd_inode));
	ret = vdi_read_inode(c, srcname, srctag, inode, false);
	if (ret != SD_RES_SUCCESS) {
		fprintf(stderr, "Failed to read inode for VDI: %s "
				"(tag: %s)\n", srcname, srctag);
		goto out;
	}

	ret = do_vdi_create(c, dstname, inode->vdi_size, inode->vdi_id,
			NULL, false, inode->store_policy);

	if (ret != SD_RES_SUCCESS)
		fprintf(stderr, "Clone vdi failed:%s\n", sd_strerr(ret));

out:
	free(inode);
	return ret;
}


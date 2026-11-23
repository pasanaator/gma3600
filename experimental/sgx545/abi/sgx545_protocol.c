// SPDX-License-Identifier: GPL-2.0-only
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif
#include "sgx545_abi.h"

int sgx545_session_close(struct sgx545_session *s,
	const struct sgx545_bridge_ops *ops, void *ctx)
{
	int ret;

	if (!s->services)
		return 0;
	ret = ops->disconnect(ctx);
	if (!ret)
		memset(s, 0, sizeof(*s));
	return ret;
}

int sgx545_bridge_execute(struct sgx545_session *s,
	const struct sgx545_bridge_ops *ops, void *ctx,
	const struct sgx545_bridge_request *r)
{
	union {
		struct sgx545_connect_out connect;
		struct sgx545_enumerate_out enumerate;
		struct sgx545_handle_out handle;
	} out = {0};
	__u32 in[3] = {0}, cmd, token = 0, out_size;
	int ret, copied, rollback = 0, created = 0;

	if ((r->bridge_id & ~0xffU) != SGX545_COMMAND_BASE)
		return -EINVAL;
	cmd = r->bridge_id & 0xffU;
	switch (cmd) {
	case SGX545_CMD_CONNECT:
		out_size = sizeof(out.connect);
		if (r->in_size != 8 || r->services)
			return -EINVAL;
		break;
	case SGX545_CMD_ACQUIRE:
		out_size = sizeof(out.handle);
		if (r->in_size != 12)
			return -EINVAL;
		break;
	case SGX545_CMD_ENUMERATE:
	case SGX545_CMD_DISCONNECT:
		out_size = cmd == SGX545_CMD_ENUMERATE ? sizeof(out.enumerate) : sizeof(out.handle);
		if (r->in_size != 0 && r->in_size != 4)
			return -EINVAL;
		break;
	default:
		return -EOPNOTSUPP;
	}
	if (r->out_size != out_size)
		return -EINVAL;
	if (cmd != SGX545_CMD_CONNECT && (!s->services || r->services != s->services))
		return -ENOENT;
	if (r->in_size) {
		ret = ops->copy_in(ctx, in, r->param_in, r->in_size);
		if (ret)
			return ret;
	}
	if (in[0] || (cmd == SGX545_CMD_CONNECT && in[1]))
		return -EOPNOTSUPP;

	switch (cmd) {
	case SGX545_CMD_CONNECT:
		if (s->services) {
			out.connect.services_lo = s->services;
			ret = 0;
		} else {
			ret = ops->connect(ctx, &token);
			if (!ret && !token) {
				ops->disconnect(ctx);
				return -EIO;
			}
			if (!ret) {
				created = 1;
				out.connect.services_lo = token;
			}
		}
		if (ret < 0)
			return ret;
		out.connect.error = ret;
		break;
	case SGX545_CMD_ENUMERATE:
		ret = ops->enumerate(ctx, &out.enumerate);
		if (ret < 0)
			return ret;
		if (out.enumerate.count > SGX545_MAX_DEVICES)
			return -EOVERFLOW;
		out.enumerate.error = ret;
		break;
	case SGX545_CMD_ACQUIRE:
		if (in[2] != SGX545_DEVICE_TYPE)
			return -EOPNOTSUPP;
		if (s->device) {
			if (in[1] != s->device_index || in[2] != s->device_type)
				return -EOPNOTSUPP;
			out.handle.handle = s->device;
			ret = 0;
		} else {
			ret = ops->acquire(ctx, in[1], in[2], &token);
			if (!ret && !token) {
				ops->release_device(ctx);
				return -EIO;
			}
			if (!ret) {
				created = 1;
				out.handle.handle = token;
			}
		}
		if (ret < 0)
			return ret;
		out.handle.error = ret;
		break;
	case SGX545_CMD_DISCONNECT:
		ret = sgx545_session_close(s, ops, ctx);
		if (ret < 0)
			return ret;
		out.handle.error = ret;
		break;
	}
	copied = ops->copy_out(ctx, r->param_out, &out, out_size);
	if (copied && created) {
		if (cmd == SGX545_CMD_CONNECT)
			rollback = ops->disconnect(ctx);
		else
			rollback = ops->release_device(ctx);
	}
	/* Preserve ownership if rollback failed, so close can retry cleanup. */
	if (created && (!copied || rollback)) {
		if (cmd == SGX545_CMD_CONNECT)
			s->services = token;
		else {
			s->device = token;
			s->device_index = in[1];
			s->device_type = in[2];
		}
	}
	return copied;
}

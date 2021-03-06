/* Copyright (c) 2014-2016, Marel
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/* Asynchronous SDO client message processor.
 *
 * Features:
 * - Converts between plain data buffers and SDO transactions.
 * - Chooses expediated/segmented mode based on data size.
 * - Automatic timeout with abort.
 * - Enforces correct communication according to standard.
 * - Validates data according to state and aborts when receiving unexpected
 *   data.
 *
 * There can be only one of these per node.
 */

#include <assert.h>
#include <mloop.h>
#include "canopen/sdo.h"
#include "canopen/sdo_async.h"
#include "canopen.h"
#include "net-util.h"
#include "sock.h"

#define MIN(a, b) ((a) < (b)) ? (a) : (b);

#define SDO_BUFFER_INITIAL_SIZE 8

#ifndef CAN_MAX_DLC
#define CAN_MAX_DLC 8
#endif

static int sdo_async__send(struct sdo_async* self, struct can_frame* cf)
{
	if (self->quirks & SDO_ASYNC_QUIRK_NEEDS_FULL_FRAME)
		cf->can_dlc = CAN_MAX_DLC;

	return sock_send(&self->sock, cf, 0);
}

int sdo_async_stop(struct sdo_async* self)
{
	int rc = -1;
	if (!self->is_running)
		return -1;

	mloop_timer_stop(self->timer);

	if (self->context && self->free_fn)
		self->free_fn(self->context);

	self->is_running = 0;

	return 0;
}

void sdo_async__on_done(struct sdo_async* self)
{
	assert(self->is_running);

	mloop_timer_stop(self->timer);

	void* context = self->context;
	sdo_async_free_fn free_fn = self->free_fn;

	self->is_running = 0;

	sdo_async_fn on_done = self->on_done;
	if (on_done)
		on_done(self);

	if (context && free_fn)
		free_fn(context);
}

static inline void sdo_async__init_frame(const struct sdo_async* self,
					 struct can_frame* cf)
{
	sdo_clear_frame(cf);
	cf->can_id = R_RSDO + self->nodeid;
}

static int sdo_async__abort(struct sdo_async* self, enum sdo_abort_code code)
{
	struct can_frame cf;
	mloop_timer_stop(self->timer);
	sdo_async__init_frame(self, &cf);
	sdo_abort(&cf, code, self->index, self->subindex);
	sdo_async__send(self, &cf);
	self->status = SDO_REQ_LOCAL_ABORT;
	self->abort_code = code;
	sdo_async__on_done(self);
	return -1;
}

void sdo_async__on_timeout(struct mloop_timer* timer)
{
	struct sdo_async* self = mloop_timer_get_context(timer);
	sdo_async__abort(self, SDO_ABORT_TIMEOUT);
}

int sdo_async_init(struct sdo_async* self, const struct sock* sock, int nodeid)
{
	memset(self, 0, sizeof(*self));

	if (vector_init(&self->buffer, SDO_BUFFER_INITIAL_SIZE) < 0)
		return -1;

	self->timer = mloop_timer_new(mloop_default());
	if (!self->timer)
		goto failure;

	self->sock = *sock;
	self->nodeid = nodeid;
	mloop_timer_set_context(self->timer, self, NULL);
	mloop_timer_set_callback(self->timer, sdo_async__on_timeout);

	return 0;

failure:
	vector_destroy(&self->buffer);
	return -1;
}

void sdo_async_destroy(struct sdo_async* self)
{
	vector_destroy(&self->buffer);
	mloop_timer_unref(self->timer);
}

static inline int sdo_async__is_expediated(const struct sdo_async* self)
{
	return self->buffer.index <= SDO_EXPEDIATED_DATA_SIZE;
}

int sdo_async__send_init_dl(struct sdo_async* self)
{
	struct can_frame cf;
	sdo_async__init_frame(self, &cf);
	sdo_set_cs(&cf, SDO_CCS_DL_INIT_REQ);
	sdo_set_index(&cf, self->index);
	sdo_set_subindex(&cf, self->subindex);
	sdo_indicate_size(&cf);
	if (sdo_async__is_expediated(self)) {
		sdo_expediate(&cf);
		sdo_set_expediated_size(&cf, self->buffer.index);
		cf.can_dlc = SDO_EXPEDIATED_DATA_IDX + self->buffer.index;
		memcpy(&cf.data[SDO_EXPEDIATED_DATA_IDX], self->buffer.data,
		       self->buffer.index);
	} else {
		sdo_set_indicated_size(&cf, self->buffer.index);
		cf.can_dlc = CAN_MAX_DLC;
	}
	mloop_timer_start(self->timer);
	sdo_async__send(self, &cf);
	return 0;
}

int sdo_async__send_init_ul(struct sdo_async* self)
{
	struct can_frame cf;
	sdo_async__init_frame(self, &cf);
	sdo_set_cs(&cf, SDO_CCS_UL_INIT_REQ);
	sdo_set_index(&cf, self->index);
	sdo_set_subindex(&cf, self->subindex);
	cf.can_dlc = 4;
	mloop_timer_start(self->timer);
	sdo_async__send(self, &cf);
	return 0;
}

int sdo_async__send_init(struct sdo_async* self)
{
	switch (self->type) {
	case SDO_REQ_DOWNLOAD: return sdo_async__send_init_dl(self);
	case SDO_REQ_UPLOAD: return sdo_async__send_init_ul(self);
	}

	abort();
	return -1;
}

int sdo_async_start(struct sdo_async* self, const struct sdo_async_info* info)
{
	if (self->is_running)
		return -1;

	self->context = info->context;
	self->free_fn = info->free_fn;
	self->pos = 0;
	self->is_toggled = 0;
	self->comm_state = SDO_ASYNC_COMM_START;
	self->type = info->type;
	self->on_done = info->on_done;
	self->index = info->index;
	self->subindex = info->subindex;
	self->is_size_indicated = 0;
	mloop_timer_set_time(self->timer, info->timeout * 1000000ULL);

	if (info->type == SDO_REQ_DOWNLOAD)
		vector_assign(&self->buffer, info->data, info->size);
	else
		vector_clear(&self->buffer);

	self->comm_state = SDO_ASYNC_COMM_INIT_RESPONSE;

	self->is_running = 1;

	sdo_async__send_init(self);

	return 0;
}

static inline int sdo_async__is_at_end(const struct sdo_async* self)
{
	return self->pos >= self->buffer.index;
}

int sdo_async__request_dl_segment(struct sdo_async* self)
{
	struct can_frame cf;
	sdo_async__init_frame(self, &cf);
	sdo_set_cs(&cf, SDO_CCS_DL_SEG_REQ);
	if (self->is_toggled) sdo_toggle(&cf);

	size_t size = MIN(SDO_SEGMENT_MAX_SIZE, self->buffer.index - self->pos);
	assert(size > 0);

	sdo_set_segment_size(&cf, size);
	memcpy(&cf.data[SDO_SEGMENT_IDX], self->buffer.data + self->pos, size);

	cf.can_dlc = SDO_SEGMENT_IDX + size;
	self->pos += size;

	if (sdo_async__is_at_end(self))
		sdo_end_segment(&cf);

	mloop_timer_start(self->timer);
	sdo_async__send(self, &cf);

	return 0;
}

int sdo_async__feed_init_dl_response(struct sdo_async* self,
				     const struct can_frame* cf)
{
	if (cf->can_dlc < 4)
		return sdo_async__abort(self, SDO_ABORT_GENERAL);

	int cs = sdo_get_cs(cf);
	int index = sdo_get_index(cf);
	int subindex = sdo_get_subindex(cf);

	if (cs != SDO_SCS_DL_INIT_RES)
		return sdo_async__abort(self, SDO_ABORT_INVALID_CS);

	if (!(self->quirks & SDO_ASYNC_QUIRK_IGNORE_MULTIPLEXER))
		if (index != self->index || subindex != self->subindex)
			return sdo_async__abort(self, SDO_ABORT_GENERAL);

	if (sdo_async__is_expediated(self)) {
		self->status = SDO_REQ_OK;
		sdo_async__on_done(self);
	} else {
		sdo_async__request_dl_segment(self);
		self->comm_state = SDO_ASYNC_COMM_SEG_RESPONSE;
	}

	return 0;
}

int sdo_async__handle_expediated_ul(struct sdo_async* self,
				    const struct can_frame* cf)
{
	self->is_size_indicated = sdo_is_size_indicated(cf);
	size_t size = self->is_size_indicated
		    ? sdo_get_expediated_size(cf)
		    : SDO_EXPEDIATED_DATA_SIZE;
	assert(size <= SDO_EXPEDIATED_DATA_SIZE);
	vector_assign(&self->buffer, &cf->data[SDO_EXPEDIATED_DATA_IDX],
		      size);
	self->status = SDO_REQ_OK;
	sdo_async__on_done(self);
	return 0;
}

int sdo_async__request_ul_segment(struct sdo_async* self)
{
	struct can_frame cf;
	sdo_async__init_frame(self, &cf);
	sdo_set_cs(&cf, SDO_CCS_UL_SEG_REQ);
	if (self->is_toggled) sdo_toggle(&cf);
	cf.can_dlc = 1;
	mloop_timer_start(self->timer);
	sdo_async__send(self, &cf);
	return 0;
}

int sdo_async__handle_init_segmented_ul(struct sdo_async* self,
					const struct can_frame* cf)
{
	self->is_size_indicated = sdo_is_size_indicated(cf);
	if (self->is_size_indicated && cf->can_dlc == CAN_MAX_DLC)
		if (vector_reserve(&self->buffer, sdo_get_indicated_size(cf)) < 0)
			return sdo_async__abort(self, SDO_ABORT_NOMEM);

	sdo_async__request_ul_segment(self);
	self->comm_state = SDO_ASYNC_COMM_SEG_RESPONSE;

	return 0;
}

int sdo_async__feed_init_ul_response(struct sdo_async* self,
				     const struct can_frame* cf)
{
	if (cf->can_dlc < 4)
		return sdo_async__abort(self, SDO_ABORT_GENERAL);

	int cs = sdo_get_cs(cf);
	int index = sdo_get_index(cf);
	int subindex = sdo_get_subindex(cf);

	if (cs != SDO_SCS_UL_INIT_RES)
		return sdo_async__abort(self, SDO_ABORT_INVALID_CS);

	if (!(self->quirks & SDO_ASYNC_QUIRK_IGNORE_MULTIPLEXER))
		if (index != self->index || subindex != self->subindex)
			return sdo_async__abort(self, SDO_ABORT_GENERAL);

	return sdo_is_expediated(cf)
	     ? sdo_async__handle_expediated_ul(self, cf)
	     : sdo_async__handle_init_segmented_ul(self, cf);
}

int sdo_async__feed_init_response(struct sdo_async* self,
				  const struct can_frame* cf)
{
	switch (self->type) {
	case SDO_REQ_DOWNLOAD: return sdo_async__feed_init_dl_response(self, cf);
	case SDO_REQ_UPLOAD: return sdo_async__feed_init_ul_response(self, cf);
	}

	abort();
	return -1;
}

int sdo_async__feed_dl_seg_response(struct sdo_async* self,
				    const struct can_frame* cf)
{
	if (cf->can_dlc < 1)
		return sdo_async__abort(self, SDO_ABORT_GENERAL);

	if (sdo_get_cs(cf) != SDO_SCS_DL_SEG_RES)
		return sdo_async__abort(self, SDO_ABORT_INVALID_CS);

	if (!sdo_async__is_at_end(self)
	 && sdo_is_toggled(cf) != self->is_toggled)
		return sdo_async__abort(self, SDO_ABORT_TOGGLE);

	self->is_toggled ^= 1;

	if (sdo_async__is_at_end(self)) {
		self->status = SDO_REQ_OK;
		sdo_async__on_done(self);
	} else {
		sdo_async__request_dl_segment(self);
	}

	return 0;
}

int sdo_async__feed_ul_seg_response(struct sdo_async* self,
				    const struct can_frame* cf)
{
	if (cf->can_dlc < 1)
		return sdo_async__abort(self, SDO_ABORT_GENERAL);

	if (sdo_get_cs(cf) != SDO_SCS_UL_SEG_RES)
		return sdo_async__abort(self, SDO_ABORT_INVALID_CS);

	if (!sdo_is_end_segment(cf) && sdo_is_toggled(cf) != self->is_toggled)
		return sdo_async__abort(self, SDO_ABORT_TOGGLE);

	self->is_toggled ^= 1;

	size_t size = sdo_get_segment_size(cf);
	const void* data = &cf->data[SDO_SEGMENT_IDX];

	if (vector_append(&self->buffer, data, size) < 0)
		return sdo_async__abort(self, SDO_ABORT_NOMEM);

	if (sdo_is_end_segment(cf)) {
		self->status = SDO_REQ_OK;
		sdo_async__on_done(self);
	} else {
		sdo_async__request_ul_segment(self);
	}

	return 0;
}

int sdo_async__feed_seg_response(struct sdo_async* self,
				 const struct can_frame* cf)
{
	switch (self->type) {
	case SDO_REQ_DOWNLOAD: return sdo_async__feed_dl_seg_response(self, cf);
	case SDO_REQ_UPLOAD: return sdo_async__feed_ul_seg_response(self, cf);
	}

	abort();
	return -1;
}

int sdo_async_feed(struct sdo_async* self, const struct can_frame* cf)
{
	assert(cf->can_id == R_TSDO + self->nodeid);

	if (!self->is_running)
		return -1;

	mloop_timer_stop(self->timer);

	if (sdo_get_cs(cf) == SDO_SCS_ABORT) {
		self->status = SDO_REQ_REMOTE_ABORT;
		self->abort_code = sdo_get_abort_code(cf);
		sdo_async__on_done(self);
		return 0;
	}

	switch (self->comm_state) {
	case SDO_ASYNC_COMM_INIT_RESPONSE:
		return sdo_async__feed_init_response(self, cf);
	case SDO_ASYNC_COMM_SEG_RESPONSE:
		return sdo_async__feed_seg_response(self, cf);
	case SDO_ASYNC_COMM_START:
		break;
	}

	abort();
	return -1;
}


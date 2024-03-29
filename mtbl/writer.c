/*
 * Copyright (c) 2012 by Internet Systems Consortium, Inc. ("ISC")
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "mtbl-private.h"
#include "vector_types.h"
#include "bytes.h"

struct mtbl_writer_options {
	mtbl_compression_type		compression_type;
	size_t				block_size;
	size_t				block_restart_interval;
};

struct mtbl_writer {
	int				fd;
	struct trailer			t;
	struct block_builder		*data;
	struct block_builder		*index;

	struct mtbl_writer_options	opt;

	ubuf				*last_key;
	uint64_t			last_offset;

	bool				closed;
	bool				pending_index_entry;
	uint64_t			pending_offset;
};

static void _mtbl_writer_finish(struct mtbl_writer *);
static void _mtbl_writer_flush(struct mtbl_writer *);
static void _write_all(int fd, const uint8_t *, size_t);
static size_t _mtbl_writer_writeblock(
	struct mtbl_writer *,
	struct block_builder *,
	mtbl_compression_type);

struct mtbl_writer_options *
mtbl_writer_options_init(void)
{
	struct mtbl_writer_options *opt;
	opt = my_calloc(1, sizeof(*opt));
	opt->compression_type = DEFAULT_COMPRESSION_TYPE;
	opt->block_size = DEFAULT_BLOCK_SIZE;
	opt->block_restart_interval = DEFAULT_BLOCK_RESTART_INTERVAL;
	return (opt);
}

void
mtbl_writer_options_destroy(struct mtbl_writer_options **opt)
{
	if (*opt) {
		free(*opt);
		*opt = NULL;
	}
}

void
mtbl_writer_options_set_compression(struct mtbl_writer_options *opt,
				    mtbl_compression_type compression_type)
{
	assert(compression_type == MTBL_COMPRESSION_NONE ||
	       compression_type == MTBL_COMPRESSION_SNAPPY ||
	       compression_type == MTBL_COMPRESSION_ZLIB);
	opt->compression_type = compression_type;
}

void
mtbl_writer_options_set_block_size(struct mtbl_writer_options *opt,
				   size_t block_size)
{
	if (block_size < MIN_BLOCK_SIZE)
		block_size = MIN_BLOCK_SIZE;
	opt->block_size = block_size;
}

void
mtbl_writer_options_set_block_restart_interval(struct mtbl_writer_options *opt,
					       size_t block_restart_interval)
{
	opt->block_restart_interval = block_restart_interval;
}

struct mtbl_writer *
mtbl_writer_init_fd(int orig_fd, const struct mtbl_writer_options *opt)
{
	struct mtbl_writer *w;
	int fd;

	fd = dup(orig_fd);
	assert(fd >= 0);
	w = my_calloc(1, sizeof(*w));
	if (opt == NULL) {
		w->opt.compression_type = DEFAULT_COMPRESSION_TYPE;
		w->opt.block_size = DEFAULT_BLOCK_SIZE;
		w->opt.block_restart_interval = DEFAULT_BLOCK_RESTART_INTERVAL;
	} else {
		memcpy(&w->opt, opt, sizeof(*opt));
	}
	w->fd = fd;
	w->last_key = ubuf_init(256);
	w->t.compression_algorithm = w->opt.compression_type;
	w->t.data_block_size = w->opt.block_size;
	w->data = block_builder_init(w->opt.block_restart_interval);
	w->index = block_builder_init(w->opt.block_restart_interval);
	return (w);
}

struct mtbl_writer *
mtbl_writer_init(const char *fname, const struct mtbl_writer_options *opt)
{
	struct mtbl_writer *w;
	int fd;

	fd = open(fname, O_WRONLY | O_CREAT | O_TRUNC | O_EXCL, 0644);
	if (fd < 0)
		return (NULL);
	w = mtbl_writer_init_fd(fd, opt);
	close(fd);
	return (w);
}

void
mtbl_writer_destroy(struct mtbl_writer **w)
{
	if (*w != NULL) {
		if (!(*w)->closed) {
			_mtbl_writer_finish(*w);
			close((*w)->fd);
		}
		block_builder_destroy(&((*w)->data));
		block_builder_destroy(&((*w)->index));
		ubuf_destroy(&(*w)->last_key);
		free(*w);
		*w = NULL;
	}
}

mtbl_res
mtbl_writer_add(struct mtbl_writer *w,
		const uint8_t *key, size_t len_key,
		const uint8_t *val, size_t len_val)
{
	assert(!w->closed);
	if (w->t.count_entries > 0) {
		if (!(bytes_compare(key, len_key,
				    ubuf_data(w->last_key), ubuf_size(w->last_key)) > 0))
		{
			return (mtbl_res_failure);
		}
	}

	size_t estimated_block_size = block_builder_current_size_estimate(w->data);
	estimated_block_size += 3*5 + len_key + len_val;

	if (estimated_block_size >= w->opt.block_size)
		_mtbl_writer_flush(w);

	if (w->pending_index_entry) {
		uint8_t enc[10];
		size_t len_enc;
		assert(block_builder_empty(w->data));
		bytes_shortest_separator(w->last_key, key, len_key);
		len_enc = mtbl_varint_encode64(enc, w->last_offset);
		/*
		fprintf(stderr, "%s: writing index entry, key= '%s' (%zd) val= %" PRIu64 "\n",
			__func__, ubuf_data(w->last_key), ubuf_size(w->last_key), w->last_offset);
		*/
		block_builder_add(w->index,
				  ubuf_data(w->last_key), ubuf_size(w->last_key),
				  enc, len_enc);
		w->pending_index_entry = false;
	}

	ubuf_reset(w->last_key);
	ubuf_append(w->last_key, key, len_key);

	w->t.count_entries += 1;
	w->t.bytes_keys += len_key;
	w->t.bytes_values += len_val;
	block_builder_add(w->data, key, len_key, val, len_val);
	return (mtbl_res_success);
}

static void
_mtbl_writer_finish(struct mtbl_writer *w)
{
	uint8_t tbuf[MTBL_TRAILER_SIZE];

	_mtbl_writer_flush(w);
	assert(!w->closed);
	w->closed = true;
	if (w->pending_index_entry) {
		/* XXX use short successor */
		uint8_t enc[10];
		size_t len_enc;
		len_enc = mtbl_varint_encode64(enc, w->last_offset);
		/*
		fprintf(stderr, "%s: writing index entry, key= '%s' (%zd) val= %" PRIu64 "\n",
			__func__, ubuf_data(w->last_key), ubuf_size(w->last_key), w->last_offset);
		*/
		block_builder_add(w->index,
				  ubuf_data(w->last_key), ubuf_size(w->last_key),
				  enc, len_enc);
		w->pending_index_entry = false;
	}
	w->t.index_block_offset = w->pending_offset;
	w->t.bytes_index_block = _mtbl_writer_writeblock(w, w->index, MTBL_COMPRESSION_NONE);

	trailer_write(&w->t, tbuf);
	_write_all(w->fd, tbuf, sizeof(tbuf));
}

static void
_mtbl_writer_flush(struct mtbl_writer *w)
{
	assert(!w->closed);
	if (block_builder_empty(w->data))
		return;
	assert(!w->pending_index_entry);
	w->t.bytes_data_blocks += _mtbl_writer_writeblock(w, w->data, w->opt.compression_type);
	w->t.count_data_blocks += 1;
	w->pending_index_entry = true;
}

static size_t
_mtbl_writer_writeblock(struct mtbl_writer *w,
			struct block_builder *b,
			mtbl_compression_type compression_type)
{
	uint8_t *raw_contents = NULL, *block_contents = NULL, *comp_contents = NULL;
	size_t raw_contents_size = 0, block_contents_size = 0, comp_contents_size = 0;
	snappy_status res;
	int zret;
	z_stream zs;

	block_builder_finish(b, &raw_contents, &raw_contents_size);

	switch (compression_type) {
	case MTBL_COMPRESSION_NONE:
		block_contents = raw_contents;
		block_contents_size = raw_contents_size;
		break;
	case MTBL_COMPRESSION_SNAPPY:
		comp_contents_size = snappy_max_compressed_length(raw_contents_size);
		comp_contents = my_malloc(comp_contents_size);
		res = snappy_compress((const char *) raw_contents, raw_contents_size,
				      (char *) comp_contents, &comp_contents_size);
		assert(res == SNAPPY_OK);
		block_contents = comp_contents;
		block_contents_size = comp_contents_size;
		break;
	case MTBL_COMPRESSION_ZLIB:
		comp_contents_size = 2 * raw_contents_size;
		comp_contents = my_malloc(comp_contents_size);
		memset(&zs, 0, sizeof(zs));
		zs.zalloc = Z_NULL;
		zs.zfree = Z_NULL;
		zs.opaque = Z_NULL;
		zret = deflateInit(&zs, Z_DEFAULT_COMPRESSION);
		assert(zret == Z_OK);
		zs.avail_in = raw_contents_size;
		zs.next_in = raw_contents;
		zs.avail_out = comp_contents_size;
		zs.next_out = comp_contents;
		zret = deflate(&zs, Z_FINISH);
		assert(zret == Z_STREAM_END);
		assert(zs.avail_in == 0);
		comp_contents_size = zs.total_out;
		zret = deflateEnd(&zs);
		assert(zret == Z_OK);
		block_contents = comp_contents;
		block_contents_size = comp_contents_size;
		break;
	}

	assert(block_contents_size < UINT_MAX);

	const uint32_t crc = htole32(mtbl_crc32c(block_contents, block_contents_size));
	const uint32_t len = htole32(block_contents_size);

	_write_all(w->fd, (const uint8_t *) &len, sizeof(len));
	_write_all(w->fd, (const uint8_t *) &crc, sizeof(crc));
	_write_all(w->fd, block_contents, block_contents_size);

	const size_t bytes_written = (sizeof(len) + sizeof(crc) + block_contents_size);
	w->last_offset = w->pending_offset;
	w->pending_offset += bytes_written;

	block_builder_reset(b);
	free(raw_contents);
	free(comp_contents);

	return (bytes_written);
}

static void
_write_all(int fd, const uint8_t *buf, size_t size)
{
	assert(size > 0);

	while (size) {
		ssize_t bytes_written;

		bytes_written = write(fd, buf, size);
		if (bytes_written < 0 && errno == EINTR)
			continue;
		if (bytes_written <= 0) {
			fprintf(stderr, "%s: write() failed: %s\n", __func__,
				strerror(errno));
			assert(bytes_written > 0);
		}
		buf += bytes_written;
		size -= bytes_written;
	}
}

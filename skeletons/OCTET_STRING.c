/*-
 * Copyright (c) 2003 Lev Walkin <vlm@lionet.info>. All rights reserved.
 * Redistribution and modifications are permitted subject to BSD license.
 */
#include <OCTET_STRING.h>
#include <assert.h>
#include <errno.h>

/*
 * OCTET STRING basic type description.
 */
static ber_tlv_tag_t asn1_DEF_OCTET_STRING_tags[] = {
	(ASN_TAG_CLASS_UNIVERSAL | (4 << 2))
};
asn1_TYPE_descriptor_t asn1_DEF_OCTET_STRING = {
	"OCTET STRING",
	asn_generic_no_constraint,
	OCTET_STRING_decode_ber,
	OCTET_STRING_encode_der,
	OCTET_STRING_print,	/* non-ascii stuff, generally */
	OCTET_STRING_free,
	0, /* Use generic outmost tag fetcher */
	asn1_DEF_OCTET_STRING_tags,
	sizeof(asn1_DEF_OCTET_STRING_tags)
	  / sizeof(asn1_DEF_OCTET_STRING_tags[0]),
	-1,	/* Both ways are fine (primitive and constructed) */
	0, 0,	/* No members */
	0	/* No specifics */
};

#define	_CH_PHASE(ctx, inc) do {	\
		if(ctx->phase == 0)	\
			ctx->step = 0;	\
		ctx->phase += inc;	\
	} while(0)
#define	NEXT_PHASE(ctx)	_CH_PHASE(ctx, +1)
#define	PREV_PHASE(ctx)	_CH_PHASE(ctx, -1)

#define	ADVANCE(num_bytes)	do {			\
		size_t num = num_bytes;			\
		buf_ptr = ((char *)buf_ptr) + num;	\
		size -= num;				\
		consumed_myself += num;			\
	} while(0)

#define	RETURN(_code)	do {			\
		rval.code = _code;		\
		rval.consumed = consumed_myself;\
		return rval;			\
	} while(0)

#define	APPEND(bufptr, bufsize)	do {					\
		size_t _bs = (bufsize);					\
		size_t _ns = ctx->step;	/* Allocated */			\
		if(_ns <= (size_t)(st->size + _bs)) {			\
			void *ptr;					\
			do { _ns = _ns ? _ns<<2 : 16; }			\
			    while(_ns <= (size_t)(st->size + _bs));	\
			ptr = REALLOC(st->buf, _ns);			\
			if(ptr) {					\
				st->buf = (uint8_t *)ptr;		\
				ctx->step = _ns;			\
			} else {					\
				RETURN(RC_FAIL);			\
			}						\
		}							\
		memcpy(st->buf + st->size, bufptr, _bs);		\
		st->size += _bs;					\
		if(st->size < 0)					\
			/* Why even care?.. JIC */			\
			RETURN(RC_FAIL);				\
		/* Convenient nul-termination */			\
		st->buf[st->size] = '\0';				\
	} while(0)

/*
 * The main reason why ASN.1 is still alive is that too much time and effort
 * is necessary for learning it more or less adequately, thus creating a gut
 * necessity to demonstrate that aquired skill everywhere afterwards.
 * No, I am not going to explain what the following stuff is.
 */
struct _stack_el {
	ber_tlv_len_t	left;	/* What's left to read */
	int	want_nulls;	/* Want null "end of content" octets? */
	int	bits_chopped;	/* Flag in BIT STRING mode */
	struct _stack_el *prev;
	struct _stack_el *next;
};
struct _stack {
	struct _stack_el *tail;
	struct _stack_el *cur_ptr;
};

static struct _stack_el *
_add_stack_el(struct _stack *st) {
	struct _stack_el *nel;

	if(st->cur_ptr && st->cur_ptr->next) {
		nel = st->cur_ptr->next;
		nel->left = 0;
		nel->want_nulls = 0;
		nel->bits_chopped = 0;
	} else {
		(void *)nel = CALLOC(1, sizeof(struct _stack_el));
		if(nel == NULL)
			return NULL;
	
		if(st->tail) {
			st->tail->next = nel;
		}
		nel->prev = st->tail;
		st->tail = nel;
	}

	st->cur_ptr = nel;

	return nel;
}

static struct _stack *
_new_stack() {
	struct _stack *st;
	(void *)st = CALLOC(1, sizeof(struct _stack));
	if(st == NULL)
		return NULL;

	return st;
}

/*
 * Decode OCTET STRING type.
 */
ber_dec_rval_t
OCTET_STRING_decode_ber(asn1_TYPE_descriptor_t *td,
	void **os_structure, void *buf_ptr, size_t size, int tag_mode) {
	OCTET_STRING_t *st = (OCTET_STRING_t *)*os_structure;
	ber_dec_rval_t rval;
	ber_dec_ctx_t *ctx;
	ber_tlv_tag_t terminal_tag;	/* Inner tag for constructed types */
	ssize_t consumed_myself = 0;
	struct _stack *stck;	/* A stack structure */
	struct _stack_el *sel;	/* Stack element */
	int tlv_constr;
	int is_bit_str = 0;	/* See below under switch(td->specifics) */
	int is_ANY_type = 0;	/* See below under switch(td->specifics) */

	ASN_DEBUG("Decoding %s as %s (%ld)",
		td->name, "OCTET STRING", (long)size);

	/*
	 * Create the string if does not exist.
	 */
	if(st == NULL) {
		(void *)st = *os_structure = CALLOC(1, sizeof(*st));
		if(st == NULL)
			RETURN(RC_FAIL);
	}

	/* Restore parsing context */
	ctx = &st->_ber_dec_ctx;

	switch((int)td->specifics) {
	case 0:
		terminal_tag = asn1_DEF_OCTET_STRING_tags[0];	/* [U4] */
		break;
	case -1:	/* BIT STRING */
		/*
		 * This is some sort of a hack.
		 * The OCTET STRING decoder is being used in BIT STRING mode.
		 */
		is_bit_str = 1;
		terminal_tag = ASN_TAG_CLASS_UNIVERSAL | (3 << 2);
		break;
	default:	/* Just in case; fall through */
	case  1:	/* ANY type */
		is_ANY_type = 1;
		terminal_tag = -1;
		break;
	}

	switch(ctx->phase) {
	case 0:
		/*
		 * Check tags.
		 */
		rval = ber_check_tags(td, ctx,
			buf_ptr, size, tag_mode,
			&ctx->left, &tlv_constr);
		if(rval.code != RC_OK) {
			RETURN(rval.code);
		}

		ASN_DEBUG("OS length is %d bytes, form %d",
			(int)ctx->left, tlv_constr);

		if(tlv_constr) {
			/*
			 * Complex operation, requires stack of expectations.
			 */
			ctx->ptr = _new_stack();
			if(ctx->ptr) {
				(void *)stck = ctx->ptr;
#if 0
				if(ctx->left < 0) {
					stck->cur_ptr->want_nulls = -ctx->left;
					stck->cur_ptr->left = -1;
				} else {
					stck->cur_ptr->want_nulls = 0;
					stck->cur_ptr->left = ctx->left;
				}
				ASN_DEBUG("+EXPECT1 left=%d wn=%d",
					stck->cur_ptr->left,
					stck->cur_ptr->want_nulls);
#endif
				if(is_bit_str) {
					/* Number of meaningless tail bits */
					APPEND("\0", 1);
				}
			} else {
				RETURN(RC_FAIL);
			}
		} else {
			/*
			 * Jump into stackless primitive decoding.
			 */
			_CH_PHASE(ctx, 3);
			if(is_ANY_type) APPEND(buf_ptr, rval.consumed);
			ADVANCE(rval.consumed);
			goto phase3;
		}

		NEXT_PHASE(ctx);
		/* Fall through */
	case 1:
	phase1:
		/*
		 * Fill the stack with expectations.
		 */
		(void *)stck = ctx->ptr;
		sel = stck->cur_ptr;
	  do {
		ber_tlv_tag_t tlv_tag;
		ber_tlv_len_t tlv_len;
		ssize_t tl, ll;

		ASN_DEBUG("fetch tag(size=%d), %sstack, left=%d, want0=%d",
			(int)size, sel?"":"!",
			sel?sel->left:0, sel?sel->want_nulls:0);
		tl = ber_fetch_tag(buf_ptr, size, &tlv_tag);
		switch(tl) {
		case -1: RETURN(RC_FAIL);
		case 0: RETURN(RC_WMORE);
		}

		tlv_constr = BER_TLV_CONSTRUCTED(buf_ptr);

		ll = ber_fetch_length(tlv_constr,
				(char *)buf_ptr + tl, size - tl, &tlv_len);
		ASN_DEBUG("Got tag=%s, tc=%d, size=%d, tl=%d, len=%d, ll=%d, {%d, %d}",
			ber_tlv_tag_string(tlv_tag), tlv_constr,
				(int)size, tl, tlv_len, ll,
			((uint8_t *)buf_ptr)[0],
			((uint8_t *)buf_ptr)[1]);
		switch(ll) {
		case -1: RETURN(RC_FAIL);
		case 0: RETURN(RC_WMORE);
		}

		if(sel && sel->want_nulls
			&& ((uint8_t *)buf_ptr)[0] == 0
			&& ((uint8_t *)buf_ptr)[1] == 0)
		{
			ADVANCE(2);
			if(is_ANY_type) APPEND("\0\0", 2);

			ASN_DEBUG("Eat EOC; wn=%d--", sel->want_nulls);

			sel->want_nulls--;
			if(sel->want_nulls == 0) {
				/* Move to the next expectation */
				sel = stck->cur_ptr = sel->prev;
				if(sel == NULL)
					break;
			}

			if(sel->want_nulls) {
				/*
				 * Simulate while(TRUE) for this loop.
				 * This is necessary to fetch the next
				 * expectation after current "end of content",
				 * for which tlv_constr is 0.
				 */
				tlv_constr = 1;
			}

			continue;
		} else if(tlv_tag != terminal_tag
				&& terminal_tag != (ber_tlv_tag_t)-1) {
			char buf[2][32];
			ber_tlv_tag_snprint(tlv_tag,
				buf[0], sizeof(buf[0]));
			ber_tlv_tag_snprint(td->tags[td->tags_count-1],
				buf[1], sizeof(buf[1]));
			ASN_DEBUG("Tag does not match expectation: %s != %s",
				buf[0], buf[1]);
			RETURN(RC_FAIL);
		}

		/*
		 * Append a new expectation.
		 */
		sel = _add_stack_el(stck);
		if(sel) {
			sel->want_nulls = (tlv_len==-1);
			sel->left = tlv_len;
			ASN_DEBUG("+EXPECT2 left=%d wn=%d",
				sel->left, sel->want_nulls);
		} else {
			RETURN(RC_FAIL);
		}

		if(is_ANY_type) APPEND(buf_ptr, tl + ll);
		ADVANCE(tl+ll);
	  } while(tlv_constr);
		if(sel == NULL) {
			/* Finished operation, "phase out" */
			_CH_PHASE(ctx, +3);
			break;
		}

		NEXT_PHASE(ctx);
		/* Fall through */
	case 2:
		(void *)stck = ctx->ptr;
		sel = stck->cur_ptr;
		ASN_DEBUG("Phase 2: Need %ld bytes, size=%ld",
			(long)sel->left, (long)size);
	    {
		ber_tlv_len_t len;

		assert(sel->left >= 0);

		len = ((ber_tlv_len_t)size < sel->left)
				? (ber_tlv_len_t)size : sel->left;
		if(len > 0) {
			if(is_bit_str && sel->bits_chopped == 0) {
				/*
				 * Finalize the previous chunk:
				 * strip down unused bits.
				 */
				st->buf[st->size-1] &= 0xff << st->buf[0];

				APPEND(((char *)buf_ptr+1), (len - 1));
				st->buf[0] = *(uint8_t *)buf_ptr;
				sel->bits_chopped = 1;
			} else {
				APPEND(buf_ptr, len);
			}
			ADVANCE(len);
			sel->left -= len;
		}

		if(sel->left) {
			RETURN(RC_WMORE);
		} else {
			sel->left = 0;
			if(sel->prev)
				sel = stck->cur_ptr = sel->prev;
			PREV_PHASE(ctx);
			goto phase1;
		}
	    }
		break;
	case 3:
	phase3:
		/*
		 * Primitive form, no stack required.
		 */
		if(size < (size_t)ctx->left) {
			APPEND(buf_ptr, size);
			ctx->left -= size;
			ADVANCE(size);
			RETURN(RC_WMORE);
		} else {
			APPEND(buf_ptr, ctx->left);
			ADVANCE(ctx->left);
			ctx->left = 0;

			NEXT_PHASE(ctx);
		}
		break;
	}

	/*
	 * BIT STRING-specific processing.
	 */
	if(is_bit_str && st->size >= 2) {
		/* Finalize BIT STRING: zero out unused bits. */
		st->buf[st->size-1] &= 0xff << st->buf[0];
	}

	ASN_DEBUG("Took %d bytes to encode %s: [%s]:%d",
		consumed_myself, td->name, st->buf, st->size);

	rval.code = RC_OK;
	rval.consumed = consumed_myself;

	return rval;
}

/*
 * Encode OCTET STRING type using DER.
 */
der_enc_rval_t
OCTET_STRING_encode_der(asn1_TYPE_descriptor_t *td, void *ptr,
	int tag_mode, ber_tlv_tag_t tag,
	asn_app_consume_bytes_f *cb, void *app_key) {
	der_enc_rval_t erval;
	OCTET_STRING_t *st = (OCTET_STRING_t *)ptr;
	int add_byte = 0;
	int is_bit_str = (td->specifics == (void *)-1);
	int is_ANY_type = (td->specifics == (void *)1);

	ASN_DEBUG("%s %s as OCTET STRING",
		cb?"Estimating":"Encoding", td->name);

	/*
	 * Canonicalize BIT STRING.
	 */
	if(is_bit_str && st->buf) {
		switch(st->size) {
		case 0: add_byte = 1; break;
		case 1: st->buf[0] = 0; break;
		default:
			/* Finalize BIT STRING: zero out unused bits. */
			st->buf[st->size-1] &= 0xff << st->buf[0];
		}
	}

	if(is_ANY_type) {
		erval.encoded = 0;
	} else {
		erval.encoded = der_write_tags(td, st->size + add_byte,
			tag_mode, tag, cb, app_key);
		if(erval.encoded == -1) {
			erval.failed_type = td;
			erval.structure_ptr = ptr;
			return erval;
		}
	}

	if(cb) {
		uint8_t zero;
		uint8_t *buf;
		int size;
		ssize_t ret;

		/* BIT STRING-aware handling */
		if(add_byte) {
			zero = 0;
			buf = &zero;
			size = 1;
		} else if(st->buf) {
			buf = st->buf;
			size = st->size;
		} else {
			assert(st->size == 0);
			buf = 0;	/* Not used */
			size = 0;
		}

		if(size) {
			ret = cb(buf, size, app_key);
			if(ret == -1) {
				erval.encoded = -1;
				erval.failed_type = td;
				erval.structure_ptr = ptr;
				return erval;
			}
		}
	}

	erval.encoded += st->size + add_byte;

	return erval;
}

int
OCTET_STRING_print(asn1_TYPE_descriptor_t *td, const void *sptr, int ilevel,
	asn_app_consume_bytes_f *cb, void *app_key) {
	static const char *h2c = "0123456789ABCDEF";
	const OCTET_STRING_t *st = (const OCTET_STRING_t *)sptr;
	char scratch[16 * 3 + 4];
	char *p = scratch;
	uint8_t *buf;
	uint8_t *end;
	size_t i;
	int ret;

	(void)td;	/* Unused argument */

	if(!st || !st->buf) return cb("<absent>", 8, app_key);

	/*
	 * Dump the contents of the buffer in hexadecimal.
	 */
	buf = st->buf;
	end = buf + st->size;
	for(i = 0; buf < end; buf++, i++) {
		if(!(i % 16) && (i || st->size > 16)) {
			if(cb(scratch, p - scratch, app_key)
			|| cb("\n", 1, app_key))
				return -1;
			for(ret = 0; ret < ilevel; ret++)
				cb(" ", 1, app_key);
			p = scratch;
		}
		*p++ = h2c[(*buf >> 4) & 0x0F];
		*p++ = h2c[*buf & 0x0F];
		*p++ = ' ';
	}

	return cb(scratch, p - scratch, app_key);
}

int
OCTET_STRING_print_ascii(asn1_TYPE_descriptor_t *td, const void *sptr,
		int ilevel, asn_app_consume_bytes_f *cb, void *app_key) {
	const OCTET_STRING_t *st = (const OCTET_STRING_t *)sptr;

	(void)td;	/* Unused argument */
	(void)ilevel;	/* Unused argument */

	if(st && st->buf) {
		return cb(st->buf, st->size, app_key);
	} else {
		return cb("<absent>", 8, app_key);
	}
}

void
OCTET_STRING_free(asn1_TYPE_descriptor_t *td, void *sptr, int contents_only) {
	OCTET_STRING_t *st = (OCTET_STRING_t *)sptr;
	struct _stack *stck = (struct _stack *)st->_ber_dec_ctx.ptr;

	if(!td || !st)
		return;

	ASN_DEBUG("Freeing %s as OCTET STRING", td->name);

	if(st->buf) {
		FREEMEM(st->buf);
	}

	/*
	 * Remove decode-time stack.
	 */
	if(stck) {
		while(stck->tail) {
			struct _stack_el *sel = stck->tail;
			stck->tail = sel->prev;
			FREEMEM(sel);
		}
		FREEMEM(stck);
	}

	if(!contents_only) {
		FREEMEM(st);
	}
}

/*
 * Conversion routines.
 */
int
OCTET_STRING_fromBuf(OCTET_STRING_t *st, const char *str, int len) {
	void *buf;

	if(st == 0 || (str == 0 && len)) {
		errno = EINVAL;
		return -1;
	}

	/*
	 * Clear the OCTET STRING.
	 */
	if(str == NULL) {
		if(st->buf)
			FREEMEM(st->buf);
		st->size = 0;
		return 0;
	}

	/* Determine the original string size, if not explicitly given */
	if(len < 0)
		len = strlen(str);

	/* Allocate and fill the memory */
	buf = MALLOC(len + 1);
	if(buf == NULL) {
		return -1;
	} else {
		st->buf = (uint8_t *)buf;
		st->size = len;
	}

	memcpy(buf, str, len);
	st->buf[st->size] = '\0';	/* Couldn't use memcpy(len+1)! */

	return 0;
}

OCTET_STRING_t *
OCTET_STRING_new_fromBuf(const char *str, int len) {
	OCTET_STRING_t *st;

	(void *)st = CALLOC(1, sizeof(*st));
	if(st && str && OCTET_STRING_fromBuf(st, str, len)) {
		free(st);
		st = NULL;
	}

	return st;
}


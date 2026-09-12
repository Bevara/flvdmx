/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / FLV demultiplexer filter.
 *
 *  FLV is a tag stream: a nine-byte header, then tags, each with an eleven
 *  byte header (type, size, timestamp in milliseconds) and a body, each
 *  followed by its own size again. Three tag types matter - audio, video and
 *  script data - and within audio and video the first byte of the body names
 *  the codec. That is the whole format, and it is why this filter parses it
 *  itself rather than through a library: the libflv it used to carry only knew
 *  AVC and AAC, which is what RTMP carries today, and none of the FLV files in
 *  the wild that this player is for - and none of the test signals - use
 *  either. They are Sorenson H.263 and MP3, the codecs Flash shipped with.
 *
 *  A demultiplexer, not a decoder: it emits one pid per stream with the codec
 *  named and the decoder configuration attached, and leaves decoding to
 *  whatever chain link claims the codec. Sorenson H.263 goes to ffmpeg-h26x,
 *  AVC to h264bsd, AAC to libfaad, MP3 to libmad or libmpg123, G.711 to
 *  libg711. Output pids are created when the first tag of each stream is seen,
 *  with the picture size read from the stream itself - the Sorenson picture
 *  header or the AVC sequence parameter set - because a raw video pid without
 *  a size is one the graph resolver routes straight into the muxer.
 *
 *  Tags are dispatched as they are read, not all at once: a 40 Mo file holds
 *  several thousand frames, and pushing them into the pid buffers in one call
 *  is what gf_filter_pid_would_block exists to prevent.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>
#include <math.h>

#define FLV_HEADER_SIZE      9
#define FLV_TAG_HEADER_SIZE  11
#define FLV_PREV_SIZE        4

#define FLV_TAG_AUDIO   8
#define FLV_TAG_VIDEO   9
#define FLV_TAG_SCRIPT  18

/* video codec ids, low nibble of the first body byte */
#define FLV_VCODEC_SORENSON  2
#define FLV_VCODEC_SCREEN    3
#define FLV_VCODEC_VP6       4
#define FLV_VCODEC_VP6A      5
#define FLV_VCODEC_SCREEN2   6
#define FLV_VCODEC_AVC       7

/* audio formats, high nibble of the first body byte */
#define FLV_ACODEC_PCM_BE    0
#define FLV_ACODEC_ADPCM     1
#define FLV_ACODEC_MP3       2
#define FLV_ACODEC_PCM_LE    3
#define FLV_ACODEC_NELLY16   4
#define FLV_ACODEC_NELLY8    5
#define FLV_ACODEC_NELLY     6
#define FLV_ACODEC_G711A     7
#define FLV_ACODEC_G711U     8
#define FLV_ACODEC_AAC       10
#define FLV_ACODEC_SPEEX     11
#define FLV_ACODEC_MP3_8K    14

/* Sorenson H.263 has no codec id in GPAC; the four character code is the pid
 * codec id, and ffmpeg-h26x maps it back to AV_CODEC_ID_FLV1. */
#define GF_CODECID_FLV1  GF_4CC('F','L','V','1')

typedef struct
{
	GF_FilterPid *ipid, *v_opid, *a_opid;
	u32 pos;
	Bool hdr_done, has_audio, has_video;

	/* onMetaData, when the file has one */
	Double md_framerate, md_duration;
	u32 md_width, md_height;

	u32 vcodec, vwidth, vheight, vframe_dur;
	u32 acodec, arate, achans, aframe_dur;
	Bool v_skipped_logged, a_skipped_logged;
} GF_FLVDMXCtx;

static u32 flv_rd24(const u8 *p) { return ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2]; }
static u32 flv_rd32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

/*
 * A bit reader for the two headers that need one: the Sorenson picture header
 * and the AVC sequence parameter set.
 */
typedef struct
{
	const u8 *data;
	u32 size, bit;
} FLVBits;

static u32 flv_bit(FLVBits *bs)
{
	u32 v;
	if ((bs->bit >> 3) >= bs->size)
		return 0;
	v = (bs->data[bs->bit >> 3] >> (7 - (bs->bit & 7))) & 1;
	bs->bit++;
	return v;
}

static u32 flv_bits(FLVBits *bs, u32 n)
{
	u32 v = 0;
	while (n--)
		v = (v << 1) | flv_bit(bs);
	return v;
}

/* H.264 exp-Golomb, unsigned and signed */
static u32 flv_ue(FLVBits *bs)
{
	u32 zeros = 0;
	while (!flv_bit(bs) && (zeros < 32) && ((bs->bit >> 3) < bs->size))
		zeros++;
	return ((1u << zeros) - 1) + flv_bits(bs, zeros);
}

static s32 flv_se(FLVBits *bs)
{
	u32 v = flv_ue(bs);
	return (v & 1) ? (s32)((v + 1) / 2) : -(s32)(v / 2);
}

/*
 * Sorenson H.263 picture header (the "flv1" of ffmpeg, "FLV_VIDEO_CODEC_H263"
 * in Adobe's spec): a 17-bit start code, 5 bits of version, 8 of temporal
 * reference, then 3 bits naming the picture format. Formats 0 and 1 spell the
 * size out in 8 or 16 bits; 2 to 6 are the fixed sizes of the day.
 */
static Bool flv_sorenson_size(const u8 *data, u32 size, u32 *w, u32 *h)
{
	static const u32 fixed_w[] = {0, 0, 352, 176, 128, 320, 160};
	static const u32 fixed_h[] = {0, 0, 288, 144, 96, 240, 120};
	FLVBits bs;
	u32 fmt;

	bs.data = data;
	bs.size = size;
	bs.bit = 0;
	if (flv_bits(&bs, 17) != 1)
		return GF_FALSE;
	flv_bits(&bs, 5); /* version */
	flv_bits(&bs, 8); /* temporal reference */
	fmt = flv_bits(&bs, 3);
	if (fmt == 0)
	{
		*w = flv_bits(&bs, 8);
		*h = flv_bits(&bs, 8);
	}
	else if (fmt == 1)
	{
		*w = flv_bits(&bs, 16);
		*h = flv_bits(&bs, 16);
	}
	else if (fmt <= 6)
	{
		*w = fixed_w[fmt];
		*h = fixed_h[fmt];
	}
	else
		return GF_FALSE;
	return (*w && *h) ? GF_TRUE : GF_FALSE;
}

/*
 * Picture size from an AVC sequence parameter set, the NAL payload after the
 * header byte. Emulation prevention bytes are removed first: a 00 00 03 in the
 * SPS is a 00 00 with a byte to keep it from looking like a start code.
 */
static Bool flv_avc_sps_size(const u8 *nal, u32 nal_size, u32 *w, u32 *h)
{
	u8 rbsp[512];
	u32 i, n = 0, profile_idc, poc_type, frame_mbs_only;
	u32 pic_w_mbs, pic_h_map;
	u32 crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0;
	u32 chroma_format_idc = 1;
	FLVBits bs;

	if (nal_size < 4)
		return GF_FALSE;
	for (i = 1; (i < nal_size) && (n < sizeof(rbsp)); i++)
	{
		if ((i + 2 < nal_size) && !nal[i] && !nal[i + 1] && (nal[i + 2] == 3))
		{
			rbsp[n++] = 0;
			rbsp[n++] = 0;
			i += 2;
			continue;
		}
		rbsp[n++] = nal[i];
	}
	bs.data = rbsp;
	bs.size = n;
	bs.bit = 0;

	profile_idc = flv_bits(&bs, 8);
	flv_bits(&bs, 8); /* constraint flags */
	flv_bits(&bs, 8); /* level */
	flv_ue(&bs);      /* seq_parameter_set_id */

	if ((profile_idc == 100) || (profile_idc == 110) || (profile_idc == 122) || (profile_idc == 244) ||
	    (profile_idc == 44) || (profile_idc == 83) || (profile_idc == 86) || (profile_idc == 118) ||
	    (profile_idc == 128) || (profile_idc == 138) || (profile_idc == 139) || (profile_idc == 134))
	{
		chroma_format_idc = flv_ue(&bs);
		if (chroma_format_idc == 3)
			flv_bit(&bs); /* separate_colour_plane_flag */
		flv_ue(&bs);  /* bit_depth_luma_minus8 */
		flv_ue(&bs);  /* bit_depth_chroma_minus8 */
		flv_bit(&bs); /* qpprime_y_zero_transform_bypass_flag */
		if (flv_bit(&bs)) /* seq_scaling_matrix_present_flag */
		{
			u32 lists = (chroma_format_idc != 3) ? 8 : 12;
			for (i = 0; i < lists; i++)
			{
				if (flv_bit(&bs))
				{
					u32 j, size = (i < 6) ? 16 : 64;
					s32 last = 8, next = 8;
					for (j = 0; j < size; j++)
					{
						if (next)
							next = (last + flv_se(&bs) + 256) % 256;
						last = next ? next : last;
					}
				}
			}
		}
	}
	flv_ue(&bs); /* log2_max_frame_num_minus4 */
	poc_type = flv_ue(&bs);
	if (poc_type == 0)
		flv_ue(&bs);
	else if (poc_type == 1)
	{
		u32 cycles;
		flv_bit(&bs);
		flv_se(&bs);
		flv_se(&bs);
		cycles = flv_ue(&bs);
		for (i = 0; (i < cycles) && (i < 256); i++)
			flv_se(&bs);
	}
	flv_ue(&bs);  /* max_num_ref_frames */
	flv_bit(&bs); /* gaps_in_frame_num_value_allowed_flag */
	pic_w_mbs = flv_ue(&bs) + 1;
	pic_h_map = flv_ue(&bs) + 1;
	frame_mbs_only = flv_bit(&bs);
	if (!frame_mbs_only)
		flv_bit(&bs); /* mb_adaptive_frame_field_flag */
	flv_bit(&bs);     /* direct_8x8_inference_flag */
	if (flv_bit(&bs)) /* frame_cropping_flag */
	{
		crop_l = flv_ue(&bs);
		crop_r = flv_ue(&bs);
		crop_t = flv_ue(&bs);
		crop_b = flv_ue(&bs);
	}

	{
		/* crop units depend on the chroma format and on interlacing */
		u32 sub_w = (chroma_format_idc == 3) ? 1 : 2;
		u32 sub_h = (chroma_format_idc == 1) ? 2 : 1;
		u32 unit_x = sub_w, unit_y = sub_h * (2 - frame_mbs_only);
		u32 width = pic_w_mbs * 16, height = (2 - frame_mbs_only) * pic_h_map * 16;
		if ((crop_l + crop_r) * unit_x >= width || (crop_t + crop_b) * unit_y >= height)
			return GF_FALSE;
		*w = width - (crop_l + crop_r) * unit_x;
		*h = height - (crop_t + crop_b) * unit_y;
	}
	return (*w && *h) ? GF_TRUE : GF_FALSE;
}

/*
 * onMetaData is AMF0: the string "onMetaData", then an ECMA array of named
 * values. Only numbers are of interest, and only four of them; everything else
 * is skipped by type so the walk stays aligned.
 */
static Bool flv_amf_skip(const u8 *d, u32 size, u32 *pos);

static Bool flv_amf_skip_pairs(const u8 *d, u32 size, u32 *pos)
{
	while (*pos + 3 <= size)
	{
		u32 klen = ((u32)d[*pos] << 8) | d[*pos + 1];
		if (!klen && (d[*pos + 2] == 9))
		{
			*pos += 3;
			return GF_TRUE;
		}
		*pos += 2 + klen;
		if (!flv_amf_skip(d, size, pos))
			return GF_FALSE;
	}
	return GF_FALSE;
}

static Bool flv_amf_skip(const u8 *d, u32 size, u32 *pos)
{
	u8 type;
	if (*pos >= size)
		return GF_FALSE;
	type = d[(*pos)++];
	switch (type)
	{
	case 0: *pos += 8; return (*pos <= size);            /* number */
	case 1: *pos += 1; return (*pos <= size);            /* boolean */
	case 2:                                              /* string */
		if (*pos + 2 > size) return GF_FALSE;
		*pos += 2 + (((u32)d[*pos] << 8) | d[*pos + 1]);
		return (*pos <= size);
	case 3: return flv_amf_skip_pairs(d, size, pos);     /* object */
	case 5: case 6: return GF_TRUE;                      /* null, undefined */
	case 8:                                              /* ECMA array */
		*pos += 4;
		return flv_amf_skip_pairs(d, size, pos);
	case 10:                                             /* strict array */
	{
		u32 i, count;
		if (*pos + 4 > size) return GF_FALSE;
		count = flv_rd32(d + *pos);
		*pos += 4;
		for (i = 0; i < count; i++)
			if (!flv_amf_skip(d, size, pos)) return GF_FALSE;
		return GF_TRUE;
	}
	case 11: *pos += 10; return (*pos <= size);          /* date */
	case 12:                                             /* long string */
		if (*pos + 4 > size) return GF_FALSE;
		*pos += 4 + flv_rd32(d + *pos);
		return (*pos <= size);
	default:
		return GF_FALSE;
	}
}

static Double flv_amf_number(const u8 *p)
{
	u64 bits = ((u64)flv_rd32(p) << 32) | flv_rd32(p + 4);
	Double v;
	memcpy(&v, &bits, 8);
	return v;
}

static void flvdmx_parse_metadata(GF_FLVDMXCtx *ctx, const u8 *d, u32 size)
{
	u32 pos = 0;
	if ((size < 13) || (d[0] != 2))
		return;
	pos = 3 + (((u32)d[1] << 8) | d[2]);
	if ((pos + 5 > size) || ((d[pos] != 8) && (d[pos] != 3)))
		return;
	pos += (d[pos] == 8) ? 5 : 1;

	while (pos + 3 <= size)
	{
		u32 klen = ((u32)d[pos] << 8) | d[pos + 1];
		const char *key;
		if (!klen && (d[pos + 2] == 9))
			break;
		key = (const char *)d + pos + 2;
		pos += 2 + klen;
		if ((pos < size) && (d[pos] == 0) && (pos + 9 <= size))
		{
			Double v = flv_amf_number(d + pos + 1);
			if ((klen == 5) && !strncmp(key, "width", 5)) ctx->md_width = (u32)v;
			else if ((klen == 6) && !strncmp(key, "height", 6)) ctx->md_height = (u32)v;
			else if ((klen == 9) && !strncmp(key, "framerate", 9)) ctx->md_framerate = v;
			else if ((klen == 8) && !strncmp(key, "duration", 8)) ctx->md_duration = v;
			pos += 9;
		}
		else if (!flv_amf_skip(d, size, &pos))
			break;
	}
}

/* MP3 frame header: enough of it to know the real sampling rate and channel
 * count. The FLV audio tag header only has a 2-bit rate field that cannot say
 * 48 kHz, which is what most MP3 in FLV actually is. */
static Bool flv_mp3_info(const u8 *d, u32 size, u32 *rate, u32 *chans)
{
	static const u32 rates[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};
	u32 version, sr_idx, mode;
	if ((size < 4) || (d[0] != 0xFF) || ((d[1] & 0xE0) != 0xE0))
		return GF_FALSE;
	version = (d[1] >> 3) & 3; /* 3: MPEG-1, 2: MPEG-2, 0: MPEG-2.5 */
	sr_idx = (d[2] >> 2) & 3;
	mode = (d[3] >> 6) & 3;
	if ((version == 1) || (sr_idx == 3))
		return GF_FALSE;
	*rate = rates[(version == 3) ? 0 : (version == 2) ? 1 : 2][sr_idx];
	*chans = (mode == 3) ? 1 : 2;
	return GF_TRUE;
}

/* AudioSpecificConfig: object type, sampling frequency index, channel config */
static Bool flv_aac_info(const u8 *d, u32 size, u32 *rate, u32 *chans)
{
	static const u32 rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
	FLVBits bs;
	u32 aot, sr_idx;
	if (size < 2)
		return GF_FALSE;
	bs.data = d;
	bs.size = size;
	bs.bit = 0;
	aot = flv_bits(&bs, 5);
	if (aot == 31)
		flv_bits(&bs, 6);
	sr_idx = flv_bits(&bs, 4);
	if (sr_idx == 15)
		*rate = flv_bits(&bs, 24);
	else if (sr_idx < 13)
		*rate = rates[sr_idx];
	else
		return GF_FALSE;
	*chans = flv_bits(&bs, 4);
	if (!*chans)
		*chans = 2;
	return (*rate != 0) ? GF_TRUE : GF_FALSE;
}

static void flvdmx_set_common(GF_FLVDMXCtx *ctx, GF_FilterPid *pid, u32 stream_type, u32 codecid)
{
	gf_filter_pid_set_property(pid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(stream_type));
	gf_filter_pid_set_property(pid, GF_PROP_PID_CODECID, &PROP_UINT(codecid));
	gf_filter_pid_set_property(pid, GF_PROP_PID_TIMESCALE, &PROP_UINT(1000));
	gf_filter_pid_set_property(pid, GF_PROP_PID_UNFRAMED, &PROP_BOOL(GF_FALSE));
	if (ctx->md_duration > 0)
		gf_filter_pid_set_property(pid, GF_PROP_PID_DURATION, &PROP_FRAC64_INT((s64)(ctx->md_duration * 1000), 1000));
}

static void flvdmx_create_video(GF_Filter *filter, GF_FLVDMXCtx *ctx, u32 codecid, u32 w, u32 h, const u8 *dsi, u32 dsi_size)
{
	Double fps = (ctx->md_framerate > 0) ? ctx->md_framerate : 25.0;
	u32 fps_num = (u32)(fps * 1000 + 0.5);

	ctx->v_opid = gf_filter_pid_new(filter);
	flvdmx_set_common(ctx, ctx->v_opid, GF_STREAM_VISUAL, codecid);
	gf_filter_pid_set_property(ctx->v_opid, GF_PROP_PID_WIDTH, &PROP_UINT(w));
	gf_filter_pid_set_property(ctx->v_opid, GF_PROP_PID_HEIGHT, &PROP_UINT(h));
	gf_filter_pid_set_property(ctx->v_opid, GF_PROP_PID_FPS, &PROP_FRAC_INT(fps_num, 1000));
	if (dsi && dsi_size)
		gf_filter_pid_set_property(ctx->v_opid, GF_PROP_PID_DECODER_CONFIG, &PROP_DATA((u8 *)dsi, dsi_size));
	ctx->vcodec = codecid;
	ctx->vwidth = w;
	ctx->vheight = h;
	ctx->vframe_dur = (u32)(1000.0 / fps + 0.5);
}

static void flvdmx_create_audio(GF_Filter *filter, GF_FLVDMXCtx *ctx, u32 codecid, u32 rate, u32 chans, u32 frame_samples, const u8 *dsi, u32 dsi_size)
{
	ctx->a_opid = gf_filter_pid_new(filter);
	flvdmx_set_common(ctx, ctx->a_opid, GF_STREAM_AUDIO, codecid);
	gf_filter_pid_set_property(ctx->a_opid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(rate));
	gf_filter_pid_set_property(ctx->a_opid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(chans));
	if (dsi && dsi_size)
		gf_filter_pid_set_property(ctx->a_opid, GF_PROP_PID_DECODER_CONFIG, &PROP_DATA((u8 *)dsi, dsi_size));
	ctx->acodec = codecid;
	ctx->arate = rate;
	ctx->achans = chans;
	ctx->aframe_dur = frame_samples ? (u32)((u64)frame_samples * 1000 / rate) : 0;
}

static void flvdmx_send(GF_FilterPid *pid, const u8 *data, u32 size, u64 cts, u64 dts, u32 dur, Bool sap)
{
	u8 *out;
	GF_FilterPacket *pck = gf_filter_pck_new_alloc(pid, size, &out);
	if (!pck)
		return;
	memcpy(out, data, size);
	gf_filter_pck_set_cts(pck, cts);
	gf_filter_pck_set_dts(pck, dts);
	if (dur)
		gf_filter_pck_set_duration(pck, dur);
	gf_filter_pck_set_sap(pck, sap ? GF_FILTER_SAP_1 : GF_FILTER_SAP_NONE);
	gf_filter_pck_set_framing(pck, GF_TRUE, GF_TRUE);
	gf_filter_pck_send(pck);
}

static void flvdmx_video_tag(GF_Filter *filter, GF_FLVDMXCtx *ctx, const u8 *body, u32 size, u32 ts)
{
	u32 frame_type, codec;
	Bool key;
	if (size < 2)
		return;
	frame_type = body[0] >> 4;
	codec = body[0] & 0x0F;
	key = (frame_type == 1) ? GF_TRUE : GF_FALSE;
	/* frame type 5 is a "video info/command frame", not a picture */
	if (frame_type == 5)
		return;

	if (codec == FLV_VCODEC_SORENSON)
	{
		if (!ctx->v_opid)
		{
			u32 w = 0, h = 0;
			if (!flv_sorenson_size(body + 1, size - 1, &w, &h))
			{
				w = ctx->md_width;
				h = ctx->md_height;
			}
			if (!w || !h)
				return;
			flvdmx_create_video(filter, ctx, GF_CODECID_FLV1, w, h, NULL, 0);
		}
		if (ctx->vcodec == GF_CODECID_FLV1)
			flvdmx_send(ctx->v_opid, body + 1, size - 1, ts, ts, ctx->vframe_dur, key);
		return;
	}

	if (codec == FLV_VCODEC_AVC)
	{
		u32 pkt_type;
		s32 cts_offset;
		if (size < 5)
			return;
		pkt_type = body[1];
		cts_offset = (s32)(flv_rd24(body + 2) << 8) >> 8;

		if (pkt_type == 0)
		{
			/* AVCDecoderConfigurationRecord, exactly what an AVC pid carries
			 * as decoder config and what h264bsd reads the parameter sets from.
			 * The size comes from the first SPS in it. */
			const u8 *avcc = body + 5;
			u32 avcc_size = size - 5, w = 0, h = 0;
			if (!ctx->v_opid && (avcc_size > 8))
			{
				u32 nsps = avcc[5] & 0x1F;
				if (nsps && (avcc_size > 10))
				{
					u32 sps_len = ((u32)avcc[6] << 8) | avcc[7];
					if (8 + sps_len <= avcc_size)
						flv_avc_sps_size(avcc + 8, sps_len, &w, &h);
				}
				if (!w || !h)
				{
					w = ctx->md_width;
					h = ctx->md_height;
				}
				if (w && h)
					flvdmx_create_video(filter, ctx, GF_CODECID_AVC, w, h, avcc, avcc_size);
			}
			return;
		}
		if ((pkt_type == 1) && ctx->v_opid && (ctx->vcodec == GF_CODECID_AVC))
		{
			u64 cts = (cts_offset < 0 && (u32)(-cts_offset) > ts) ? 0 : (u64)((s64)ts + cts_offset);
			flvdmx_send(ctx->v_opid, body + 5, size - 5, cts, ts, ctx->vframe_dur, key);
		}
		return;
	}

	if (!ctx->v_skipped_logged)
	{
		ctx->v_skipped_logged = GF_TRUE;
		GF_LOG(GF_LOG_WARNING, GF_LOG_CONTAINER, ("[FLVDmx] Video codec %u (%s) has no decoder here, video ignored\n", codec,
		                                           (codec == FLV_VCODEC_VP6) ? "VP6" : (codec == FLV_VCODEC_VP6A) ? "VP6 with alpha" : (codec == FLV_VCODEC_SCREEN) || (codec == FLV_VCODEC_SCREEN2) ? "Screen video" : "unknown"));
	}
}

static void flvdmx_audio_tag(GF_Filter *filter, GF_FLVDMXCtx *ctx, const u8 *body, u32 size, u32 ts)
{
	static const u32 flv_rates[] = {5512, 11025, 22050, 44100};
	u32 format, rate, chans;
	if (size < 2)
		return;
	format = body[0] >> 4;
	rate = flv_rates[(body[0] >> 2) & 3];
	chans = (body[0] & 1) ? 2 : 1;

	switch (format)
	{
	case FLV_ACODEC_MP3:
	case FLV_ACODEC_MP3_8K:
		if (!ctx->a_opid)
		{
			u32 r = 0, c = 0;
			if (flv_mp3_info(body + 1, size - 1, &r, &c))
			{
				rate = r;
				chans = c;
			}
			else if (format == FLV_ACODEC_MP3_8K)
				rate = 8000;
			flvdmx_create_audio(filter, ctx, GF_CODECID_MPEG_AUDIO, rate, chans, 1152, NULL, 0);
		}
		if (ctx->acodec == GF_CODECID_MPEG_AUDIO)
			flvdmx_send(ctx->a_opid, body + 1, size - 1, ts, ts, ctx->aframe_dur, GF_TRUE);
		return;

	case FLV_ACODEC_AAC:
		if (body[1] == 0)
		{
			/* AudioSpecificConfig: the decoder config of an AAC pid */
			u32 r = 0, c = 0;
			if (!ctx->a_opid && (size > 2) && flv_aac_info(body + 2, size - 2, &r, &c))
				flvdmx_create_audio(filter, ctx, GF_CODECID_AAC_MPEG4, r, c, 1024, body + 2, size - 2);
			return;
		}
		if (ctx->a_opid && (ctx->acodec == GF_CODECID_AAC_MPEG4))
			flvdmx_send(ctx->a_opid, body + 2, size - 2, ts, ts, ctx->aframe_dur, GF_TRUE);
		return;

	case FLV_ACODEC_G711A:
	case FLV_ACODEC_G711U:
	{
		u32 codecid = (format == FLV_ACODEC_G711A) ? GF_CODECID_ALAW : GF_CODECID_MULAW;
		if (!ctx->a_opid)
			flvdmx_create_audio(filter, ctx, codecid, 8000, chans, 0, NULL, 0);
		if (ctx->acodec == codecid)
			flvdmx_send(ctx->a_opid, body + 1, size - 1, ts, ts, (size - 1) * 1000 / (8000 * chans), GF_TRUE);
		return;
	}

	case FLV_ACODEC_SPEEX:
		if (!ctx->a_opid)
			flvdmx_create_audio(filter, ctx, GF_CODECID_SPEEX, 16000, 1, 320, NULL, 0);
		if (ctx->acodec == GF_CODECID_SPEEX)
			flvdmx_send(ctx->a_opid, body + 1, size - 1, ts, ts, ctx->aframe_dur, GF_TRUE);
		return;

	default:
		if (!ctx->a_skipped_logged)
		{
			ctx->a_skipped_logged = GF_TRUE;
			GF_LOG(GF_LOG_WARNING, GF_LOG_CONTAINER, ("[FLVDmx] Audio format %u (%s) has no decoder here, audio ignored\n", format,
			                                           (format <= FLV_ACODEC_PCM_LE) ? "PCM/ADPCM" : (format <= FLV_ACODEC_NELLY) ? "Nellymoser" : "unknown"));
		}
		return;
	}
}

static GF_Err flvdmx_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_FLVDMXCtx *ctx = gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->v_opid) gf_filter_pid_remove(ctx->v_opid);
		if (ctx->a_opid) gf_filter_pid_remove(ctx->a_opid);
		ctx->v_opid = ctx->a_opid = NULL;
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);
	/* Output pids are created from the tags, once the codecs and the picture
	 * size are known; nothing is declared here. */
	return GF_OK;
}

static GF_Err flvdmx_process(GF_Filter *filter)
{
	GF_FLVDMXCtx *ctx = gf_filter_get_udta(filter);
	GF_FilterPacket *pck;
	const u8 *data;
	u32 size;

	if (!ctx->ipid)
		return GF_OK;

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			if (ctx->v_opid) gf_filter_pid_set_eos(ctx->v_opid);
			if (ctx->a_opid) gf_filter_pid_set_eos(ctx->a_opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = gf_filter_pck_get_data(pck, &size);
	if (!data)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_IO_ERR;
	}

	if (!ctx->hdr_done)
	{
		u32 data_offset;
		if ((size < FLV_HEADER_SIZE + FLV_PREV_SIZE) || memcmp(data, "FLV", 3) || (data[3] != 1))
		{
			gf_filter_pid_drop_packet(ctx->ipid);
			GF_LOG(GF_LOG_ERROR, GF_LOG_CONTAINER, ("[FLVDmx] Not an FLV file\n"));
			return GF_NON_COMPLIANT_BITSTREAM;
		}
		ctx->has_audio = (data[4] & 4) ? GF_TRUE : GF_FALSE;
		ctx->has_video = (data[4] & 1) ? GF_TRUE : GF_FALSE;
		data_offset = flv_rd32(data + 5);
		if (data_offset < FLV_HEADER_SIZE)
			data_offset = FLV_HEADER_SIZE;
		ctx->pos = data_offset + FLV_PREV_SIZE;
		ctx->hdr_done = GF_TRUE;
	}

	while (ctx->pos + FLV_TAG_HEADER_SIZE <= size)
	{
		const u8 *tag = data + ctx->pos;
		u32 type = tag[0];
		u32 body_size = flv_rd24(tag + 1);
		u32 ts = flv_rd24(tag + 4) | ((u32)tag[7] << 24);
		const u8 *body = tag + FLV_TAG_HEADER_SIZE;

		if (ctx->pos + FLV_TAG_HEADER_SIZE + body_size > size)
			break;

		/* Both streams have to be able to take a packet: a tag of either kind
		 * may come next, and returning here is what keeps a long file from
		 * being pushed into the pid buffers in one go. GPAC calls back once
		 * the downstream has drained. */
		if ((ctx->v_opid && gf_filter_pid_would_block(ctx->v_opid)) ||
		    (ctx->a_opid && gf_filter_pid_would_block(ctx->a_opid)))
			return GF_OK;

		if (type == FLV_TAG_SCRIPT)
			flvdmx_parse_metadata(ctx, body, body_size);
		else if ((type == FLV_TAG_VIDEO) && ctx->has_video)
			flvdmx_video_tag(filter, ctx, body, body_size, ts);
		else if ((type == FLV_TAG_AUDIO) && ctx->has_audio)
			flvdmx_audio_tag(filter, ctx, body, body_size, ts);

		ctx->pos += FLV_TAG_HEADER_SIZE + body_size + FLV_PREV_SIZE;
	}

	gf_filter_pid_drop_packet(ctx->ipid);
	if (!ctx->v_opid && !ctx->a_opid)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CONTAINER, ("[FLVDmx] No stream with a codec this player decodes\n"));
		return GF_NOT_SUPPORTED;
	}
	if (ctx->v_opid) gf_filter_pid_set_eos(ctx->v_opid);
	if (ctx->a_opid) gf_filter_pid_set_eos(ctx->a_opid);
	return GF_EOS;
}

static const char *flvdmx_probe_data(const u8 *data, u32 size, GF_FilterProbeScore *score)
{
	if (size < FLV_HEADER_SIZE)
		return NULL;
	if (memcmp(data, "FLV", 3) || (data[3] != 1))
	{
		*score = GF_FPROBE_NOT_SUPPORTED;
		return NULL;
	}
	*score = GF_FPROBE_SUPPORTED;
	return "video/x-flv";
}

static const GF_FilterCapability FLVDMXCaps[] = {
	CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
	CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "flv"),
	CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "video/x-flv|video/flv"),
	CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
	CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
};

GF_FilterRegister FLVDMXRegister = {
	.name = "flvdmx",
	GF_FS_SET_DESCRIPTION("FLV demultiplexer")
	GF_FS_SET_HELP("This filter demultiplexes FLV (Flash Video) files into one pid per stream, with the codec named and the decoder configuration attached: Sorenson H.263 and AVC video, MP3, AAC, G.711 and Speex audio. VP6, Screen Video and Nellymoser have no decoder here and are skipped with a warning.")
	.private_size = sizeof(GF_FLVDMXCtx),
	SETCAPS(FLVDMXCaps),
	.configure_pid = flvdmx_configure_pid,
	.process = flvdmx_process,
	.probe_data = flvdmx_probe_data,
	.hint_class_type = GF_FS_CLASS_DEMULTIPLEXER
};

const GF_FilterRegister * EMSCRIPTEN_KEEPALIVE flvdmx_register(GF_FilterSession *session)
{
	return &FLVDMXRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_flvdmx(void) {
    gf_filter_auto_register("flvdmx", flvdmx_register);
}

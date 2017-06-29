/*
 * Copyright 2015, Björn Ståhl
 * License: 3-Clause BSD, see COPYING file in the senseye source repository.
 * Reference: http://senseye.arcan-fe.com
 * Description: Translator that wraps the capstone disassembly framework.  The
 * color grouping is rather primitive, possibly research a more well thought
 * out palette that can also handle individual operands and OS specific
 * options. The overlay attempts to show coverage of the current disassembly
 * range, colored by instruction group.
 */
#include <arcan_shmif.h>
#include "libsenseye.h"
#include <inttypes.h>
#include <getopt.h>
#include <math.h>
#include <capstone/capstone.h>
#include "font_8x8.h"

/* support for a mnemonic lookup database for a hint window,
 * still incomplete
 * platform TEXT, mnem TEXT, description TEXT
 */
#ifdef DBHINT_SUPPORT
 #include <sqlite3.h>
static bool got_dbhint;
static sqlite3* dbh;
void open_dbhint(const char* fname)
{
	if (sqlite3_open_v2(fname, &dbh, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
		return;

	got_dbhint = true;
}

bool lookup_mnemonic(const char* arch, const char* mnen, const char** outdescr)
{
	bool found = false;
	static const char dql[] = "SELECT description FROM instructions "
		"WHERE ARCH = ? AND mnen LIKE ?;";
	sqlite3_stmt* stmt;
	sqlite3_prepare_v2(dbh, dql, sizeof(dql)-1, &stmt, NULL);
	sqlite3_bind_text(stmt, 1, arch, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, mnen, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(stmt) == SQLITE_ROW){
		const unsigned char* arg = sqlite3_column_text(stmt, 0);
		if (arg)
			*outdescr = strdup((const char*)arg);
		else
			*outdescr = NULL;
		found = true;
	}
	sqlite3_finalize(stmt);
	return found;
}
#endif

static const shmif_pixel col_err = SHMIF_RGBA(0xff, 0x00, 0x00, 0xff);
static const shmif_pixel col_bg  = SHMIF_RGBA(0x00, 0x00, 0x00, 0xff);
static char* fmtstr = "%p:%c%t%r%n";
static long ts = 80;

static const shmif_pixel insn_lut[] = {
	SHMIF_RGBA(0xff, 0x00, 0x00, 0xff), /* GRP_INVALID */
	SHMIF_RGBA(0xff, 0xff, 0x00, 0xff), /* GRP_JUMP */
	SHMIF_RGBA(0xaa, 0xaa, 0x00, 0xff), /* GRP_CALL */
	SHMIF_RGBA(0x00, 0xff, 0xff, 0xff), /* GRP_RET */
	SHMIF_RGBA(0xff, 0x00, 0xff, 0xff), /* GRP_INT */
	SHMIF_RGBA(0x00, 0xaa, 0xaa, 0xff), /* GRP_IRET */
};

/*
 * per-process options
 */
enum color_mode {
	COLOR_NONE = 0,
	COLOR_SIMPLE,
	COLOR_GROUP
};

enum interp_mode {
	INTERP_NORMAL = 0,
	INTERP_GSTAT
};

const char* interp_lut[] = {
	"normal",
	"gstat"
};

static cs_arch arch = CS_ARCH_ARM;
static cs_mode mode = CS_MODE_THUMB;
static cs_opt_value syntax = CS_OPT_SYNTAX_DEFAULT;
enum color_mode cmode = COLOR_SIMPLE;
static bool detail = CS_OPT_OFF;

/*
 * per-sesion options
 */
struct cs_ctx {
	int disass_ofs;
	csh handle;
	enum interp_mode mode;
	uint64_t pos;
	bool active;

	cs_insn* last;
	size_t last_count;

	struct {
		bool dirty;
		int zoom_ofs[4];
		float b_w, b_h, d_w, d_h;
		struct xlt_session* sess;
		struct arcan_shmif_cont* overlay;
	} ol;
};

static inline shmif_pixel opcode_color(cs_insn* m)
{
	switch (cmode){
	case COLOR_NONE:
	return SHMIF_RGBA(0xff, 0xff, 0xff, 0xff);
	case COLOR_GROUP:{
		int gind = 0;
		if (!m->detail)
			return SHMIF_RGBA(0xff, 0xaa, 0xff, 0xff);

		for (size_t i = 0; i < m->detail->groups_count; i++)
			gind += m->detail->groups[i];

		 return insn_lut[gind % (sizeof(insn_lut)/sizeof(insn_lut[0]))];
	}
	case COLOR_SIMPLE:
	default:
	return SHMIF_RGBA(0xff, 0xaa, 0xff, 0xff);
	}
}

static inline shmif_pixel raw_color()
{
	switch (cmode){
	case COLOR_NONE:
	return SHMIF_RGBA(0xff, 0xff, 0xff, 0xff);
	case COLOR_GROUP:
	case COLOR_SIMPLE:
	default:
	return SHMIF_RGBA(0xff, 0xff, 0xaa, 0xff);
	}
}

static inline shmif_pixel oper_color(cs_insn* m)
{
	switch (cmode){
	case COLOR_NONE:
	return SHMIF_RGBA(0xff, 0xff, 0xff, 0xff);
	case COLOR_GROUP:
	case COLOR_SIMPLE:
	default:
	return SHMIF_RGBA(0xaa, 0xff, 0xff, 0xff);
	}
}

static inline shmif_pixel position_color()
{
	switch (cmode){
	case COLOR_NONE:
	return SHMIF_RGBA(0xff, 0xff, 0xff, 0xff);

	case COLOR_GROUP:
	case COLOR_SIMPLE:
	default:
	return SHMIF_RGBA(0xaa, 0xff, 0xaa, 0xff);
	}
}

static bool over_pop(bool newdata, struct arcan_shmif_cont* in,
	int zoom_ofs[4], struct arcan_shmif_cont* over,
	struct arcan_shmif_cont* out, uint64_t pos,
	size_t buf_sz, uint8_t* buf, struct xlt_session* sess)
{
	struct cs_ctx* ctx = out->user;

/* standard calculation for finding active region and scale factors */
	float w = zoom_ofs[2] - zoom_ofs[0];
	float h = zoom_ofs[3] - zoom_ofs[1];

	float b_w = (float)over->w / w;
	float b_h = (float)over->h / h;
	float d_w = ceil(b_w);
	float d_h = ceil(b_h);

	memset(over->vidp, '\0', sizeof(shmif_pixel) * over->h * over->pitch);

	for (size_t i = 0; i < ctx->last_count; i++){
		size_t x1, y1, x2, y2;
		cs_insn* cur = &ctx->last[i];
		uint64_t addr = cur->address - pos;
		shmif_pixel col = opcode_color(cur);
		xlt_ofs_coord(sess, addr, &x1, &y1);
		xlt_ofs_coord(sess, addr + cur->size, &x2, &y2);

		while ((x1 != x2 || y1 != y2) && y1 <= zoom_ofs[3] && y1 <= y2){
			if (x1 >= zoom_ofs[0] && y1 >= zoom_ofs[1] &&
				x1 <= zoom_ofs[2] && y1 <= zoom_ofs[3])
					draw_box(over, (x1 - zoom_ofs[0]) * b_w,
					(y1 - zoom_ofs[1]) * b_h, d_w, d_h, col);

			x1++;
			if (x1 >= zoom_ofs[2]){
				x1 = zoom_ofs[0]; y1++;
			}
		}
	}

	return true;
}

static bool input(struct arcan_shmif_cont* out, arcan_event* ev)
{
	if (ev->io.datatype == EVENT_IDATATYPE_DIGITAL && out->user){
		struct cs_ctx* ctx = out->user;
		if (strcmp(ev->io.label, "RIGHT") == 0)
			ctx->disass_ofs++;
		else if (strcmp(ev->io.label, "LEFT") == 0)
			ctx->disass_ofs = ctx->disass_ofs> 0 ? ctx->disass_ofs- 1 : 0;
		else if (strcmp(ev->io.label, "TAB") == 0)
			ctx->mode = ctx->mode == 1 ? 0 : 1;
		else
			return false;
	}
	else
		return false;

	return true;
}

static inline void flush(struct arcan_shmif_cont* c,
	char* buf, size_t* ofs, size_t* xp, size_t y, shmif_pixel col)
{
	if (*ofs == 0)
		return;

	buf[*ofs] = '\0';
	draw_text(c, buf, *xp, y, col);
	*xp += *ofs * fontw;
	*ofs = 0;
}

/*
 * expand format string into buffer, render and color as we go along
 */
static char hlut[16] = {'0', '1', '2', '3', '4', '5',
	'6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

static inline void draw_mnemonic(
	struct arcan_shmif_cont* cont, struct cs_ctx* inh,
	cs_insn* m, size_t* xpos, size_t* yofs)
{
	if (cont->addr->w / fontw == 0)
		return;

/* anything above this in one pass will force flush */
	size_t csz = 64;
	char buf[64];
	size_t ofs = 0;

	char* pos = fmtstr;
	bool inctx = false;
	shmif_pixel col = SHMIF_RGBA(0xff, 0xff, 0xff, 0xff);

#define FLUSH() flush(cont, buf, &ofs, xpos, *yofs, col)

	while(*pos){
		if (ofs > (csz >> 1))
			FLUSH();

		if (*pos == '%'){
			inctx = true;
			goto step;
		}
		if (inctx){
			switch (*pos){
			case '%' : buf[ofs++] = '%'; break;
			case 't' :
				FLUSH();
				*xpos += *xpos % ts == 0 ? ts : (ts - *xpos % ts);
			break;
			case 'n' :
				FLUSH();
				*xpos = 0;
				*yofs += fonth + 2;
			break;
			case 'P' :
				if (position_color() != col)
					FLUSH();
				col = position_color();
				ofs += snprintf(&buf[ofs], csz - ofs,
					"%.4"PRIx64" ", m->address - inh->pos);
				if (ofs > csz-1)
					ofs = csz - 1;
			case 'p' :
				if (position_color() != col)
					FLUSH();

				col = position_color();
				ofs += snprintf(&buf[ofs], csz - ofs, "%.8"PRIx64" ", m->address);
				if (ofs > csz-1)
					ofs = csz-1;
			break;

			case 'x' :
				if (raw_color() != col)
					FLUSH();

				col = raw_color();

				if (ofs < csz - 4)
				for (size_t i = 0; i < m->size; i++){
					buf[ofs++] = hlut[m->bytes[i] >> 4 & 0xf];
					buf[ofs++] = hlut[m->bytes[i] >> 0 & 0xf];
					buf[ofs++] = ' ';
				}

			break;
			case 'c' :
				if (opcode_color(m) != col)
					FLUSH();

				col = opcode_color(m);
				ofs += snprintf(&buf[ofs], csz - ofs, "%s", m->mnemonic);
			break;

			case 'r' :
				if (oper_color(m) != col)
					FLUSH();

				col = oper_color(m);
				ofs += snprintf(&buf[ofs], csz - ofs, "%s", m->op_str);
			break;
			}
			inctx = false;
			goto step;
		}
		buf[ofs++] = *pos;
step:
		pos++;
	}

	FLUSH();
#undef FLUSH
}

static void draw_header(struct arcan_shmif_cont* out,
	struct cs_ctx* actx, uint64_t pos)
{
	size_t buf_sz = (out->addr->w - 4) / (fontw+2);
	if (buf_sz <= 1)
		return;

	char chbuf[buf_sz];
	snprintf(chbuf, buf_sz, "%s @ %"PRIx64" +%d",
		interp_lut[actx->mode], pos, actx->disass_ofs);
	draw_box(out, 0, 0,
		out->addr->w, fonth+2, SHMIF_RGBA(0x44, 0x44, 0x44, 0xff));
	draw_text(out, chbuf, 2, 2, SHMIF_RGBA(0xff, 0xff, 0xff, 0xff));
}

static void group_disass(struct arcan_shmif_cont* c, cs_insn* in, size_t cnt)
{
/* sweep all instructions,
 * get statistics for each group,
 * draw as group label + frequency
 */
}

static bool populate(bool newdata, struct arcan_shmif_cont* in,
	struct arcan_shmif_cont* out, uint64_t pos, size_t buf_sz, uint8_t* buf)
{
	if (!buf)
		return false;

	if (!out->user){
		out->user = malloc(sizeof(struct cs_ctx));
		memset(out->user, '\0', sizeof(struct cs_ctx));
		struct cs_ctx* inh = out->user;
		cs_err err = cs_open(arch, mode, &inh->handle);

		inh->active = true;

		if (err){
			arcan_shmif_resize(out, 256, 16);
			draw_box(out, 0, 0, 256, fonth + 6, SHMIF_RGBA(0x00, 0x00, 0x00, 0xff));
			draw_text(out, "Failed to initialize capstone (%d:%d)",
				2, 2, SHMIF_RGBA(0xff, 0x00, 0x00, 0xff));
			inh->active = false;
			return true;
		}

		cs_option(inh->handle, CS_OPT_DETAIL, detail ? CS_OPT_ON : CS_OPT_OFF);
		cs_option(inh->handle, CS_OPT_SYNTAX, syntax);
		arcan_shmif_resize(out, 256, 256);
	}

	struct cs_ctx* inh = out->user;
	if (!inh->active)
		return false;

	if (newdata)
		inh->disass_ofs = 0;

	if (buf_sz > inh->disass_ofs){
		buf_sz -= inh->disass_ofs;
		buf += inh->disass_ofs;
	}

	draw_box(out, 0, 0, out->addr->w, out->addr->h, col_bg);
	if (inh->last)
		cs_free(inh->last, inh->last_count);

	inh->last_count = cs_disasm(inh->handle,
		buf, buf_sz, pos + inh->disass_ofs, 0, &inh->last);

	if (!inh->last_count){
		char txtbuf[64];
		snprintf(txtbuf, 64, "Failed disassembly @%"PRIx64, pos);
		draw_text(out, txtbuf, 2, fonth + 4, col_err);
		goto done;
	}

	draw_box(out, 0, 0, out->addr->w, out->addr->h, col_bg);

	if (inh->mode == INTERP_NORMAL){
		size_t row = 4 + fonth, xp = 0;
		inh->pos = pos;
		for (size_t i = 0; i < inh->last_count && row < out->addr->h - fonth; i++)
			draw_mnemonic(out, inh, &inh->last[i], &xp, &row);
	}
	else
		group_disass(out, inh->last, inh->last_count);

done:
	draw_header(out, inh, pos);
	return true;
}

static struct {
	const char* name;
	enum cs_arch arch;
	enum cs_mode mode;
} archs[CS_ARCH_MAX*4+1];

/*
 * string-encode valid permutations of
 * architecture and options for -a argument
 */
static void setup_arch_lut()
{
	size_t arch_count = 0;

	for (size_t i = 0; i < CS_ARCH_MAX; i++){
		if (cs_support(i))
			switch(i){
			case CS_ARCH_ARM:
				archs[arch_count  ].name = "arm";
				archs[arch_count++].arch = i;
				archs[arch_count  ].name = "arm-thumb";
				archs[arch_count  ].mode = CS_MODE_THUMB;
				archs[arch_count++].arch = i;
				archs[arch_count  ].name = "arm-v8";
				archs[arch_count  ].mode = CS_MODE_V8;
				archs[arch_count++].arch = i;
				archs[arch_count  ].name = "arm-mclass";
				archs[arch_count  ].mode = CS_MODE_MCLASS;
				archs[arch_count++].arch = i;
			break;
			case CS_ARCH_ARM64:
				archs[arch_count  ].name = "arm64";
				archs[arch_count++].arch = i;
			break;
			case CS_ARCH_MIPS:
				archs[arch_count  ].name = "mips";
				archs[arch_count++].arch = i;
				archs[arch_count  ].name = "mips-micro";
				archs[arch_count  ].mode = CS_MODE_MICRO;
				archs[arch_count++].arch = i;
				archs[arch_count  ].name = "mips-3";
				archs[arch_count  ].mode = CS_MODE_MIPS3;
				archs[arch_count++].arch = i;
				archs[arch_count  ].name = "mips-32r6";
				archs[arch_count  ].mode = CS_MODE_MIPS32R6;
				archs[arch_count++].arch = i;
				archs[arch_count  ].name = "mips-gp64";
				archs[arch_count  ].mode = CS_MODE_MIPSGP64;
				archs[arch_count++].arch = i;
				archs[arch_count  ].name = "mips-32";
				archs[arch_count  ].mode = CS_MODE_MIPS32;
				archs[arch_count++].arch = i;
				archs[arch_count  ].name = "mips-64";
				archs[arch_count  ].mode = CS_MODE_MIPS32;
				archs[arch_count++].arch = i;
			break;
			case CS_ARCH_X86:
				archs[arch_count  ].name = "x86";
				archs[arch_count++].arch = i;
				archs[arch_count  ].name = "x86-16";
				archs[arch_count  ].mode = CS_MODE_16;
				archs[arch_count++].arch = i;
				archs[arch_count  ].name = "x86-32";
				archs[arch_count  ].mode = CS_MODE_32;
				archs[arch_count++].arch = i;
				archs[arch_count  ].name = "x86-64";
				archs[arch_count  ].mode = CS_MODE_64;
				archs[arch_count++].arch = i;
			break;
			case CS_ARCH_PPC:
				archs[arch_count  ].name = "ppc";
				archs[arch_count++].arch = i;
				archs[arch_count  ].name = "ppc-64";
				archs[arch_count  ].mode = CS_MODE_64;
				archs[arch_count++].arch = i;
			break;
			case CS_ARCH_SPARC:
				archs[arch_count  ].name = "sparc";
				archs[arch_count++].arch = i;
				archs[arch_count  ].name = "sparc-v9";
				archs[arch_count  ].mode = CS_MODE_V9;
				archs[arch_count++].arch = i;
			break;
			case CS_ARCH_SYSZ:
				archs[arch_count  ].name = "sysz";
				archs[arch_count++].arch = i;
			break;
			case CS_ARCH_XCORE:
				archs[arch_count  ].name = "xcore";
				archs[arch_count++].arch = i;
			break;
			}
	}
}

static const struct option longopts[] = {
	{"arch",   required_argument, NULL, 'a'},
	{"syntax", required_argument, NULL, 's'},
	{"color",  required_argument, NULL, 'c'},
	{"format", required_argument, NULL, 'f'},
	{"tab",    required_argument, NULL, 't'},
	{NULL, no_argument, NULL, 0}
};

static int usage()
{
	printf("Usage: xlt_capstone -a architecture [options]\n\n"
		"-a,--arch=    \tspecify architecture/mode (obligatory)\n"
		"-s,--syntax=  \tspecify disassembly syntax "
			"(opts: intel, at&t)\n"
		"-c,--color=   \tset coloring mode (default: simple)\n"
			"\tsimple, group, none\n"
		"-t,--tab=     \tset tab column width (pixels)\n"
		"-l,--loop     \tsleep/retry connection loop\n"
		"-f,--format=  \toutput format string "
			"(default: %%c%%t%%r;%%n)\n"
			"\t%%p: pos, %%P: rel-pos, %%x: raw hex %%c: opcode, \n"
			"\t%%r: operands %%n: linefeed, %%t column-align \n\n"
	);

	printf("Supported architectures:\n\t");
	for (size_t i = 0; i < sizeof(archs) / sizeof(archs[0]) && archs[i].name;i++)
		printf("%s%s ", i % 5 == 4 ? "\n\t" : "", archs[i].name);

	printf("\n");

	return EXIT_SUCCESS;
}

static int find_arch(const char* key)
{
	for (size_t i = 0; i < sizeof(archs) / sizeof(archs[0]) && archs[i].name;i++)
		if (strcmp(archs[i].name, key) == 0)
			return i;

	return -1;
}

int main(int argc, char** argv)
{
	setup_arch_lut();
	int aind = -1, ch;
	enum ARCAN_FLAGS confl = SHMIF_ACQUIRE_FATALFAIL;

	while ((ch = getopt_long(argc, argv, "a:s:c:f:t:", longopts, NULL)) >= 0){
	switch (ch){
	case 'a' : aind = find_arch(optarg); break;
	case 'f' : fmtstr = strdup(optarg); break;
	case 'l' : confl = SHMIF_CONNECT_LOOP; break;
	case 's' :
		if (strcmp(optarg, "intel") == 0)
			syntax = CS_OPT_SYNTAX_INTEL;
		else if (strcmp(optarg, "at&t") == 0)
			syntax = CS_OPT_SYNTAX_DEFAULT;
		else{
			printf("unknown syntax option (%s), supported: intel, at&t\n", optarg);
			return EXIT_FAILURE;
		}
	break;
	case 't' :
		ts = strtol(optarg, NULL, 10);
		if (ts <= 0)
			ts = 80;

	case 'c' :
		if (strcmp(optarg, "simple") == 0){
			cmode = COLOR_SIMPLE;
		}
		else if (strcmp(optarg, "group") == 0){
			cmode = COLOR_GROUP;
			detail = true;
		}
		else if (strcmp(optarg, "none") == 0){
			cmode = COLOR_NONE;
		}
		else{
			printf("unknown coloring mode (%s), "
				"supported: simple, group, none\n", optarg);
			return EXIT_FAILURE;
		}
	break;
	}
	}

	if (-1 == aind)
		return usage();

	arch = archs[aind].arch;
	mode = archs[aind].mode;

	struct xlt_context* ctx = xlt_open(archs[aind].name, XLT_DYNSIZE, confl);
	if (!ctx)
		return EXIT_FAILURE;

	xlt_config(ctx, populate, input, over_pop, NULL);
	xlt_wait(ctx);
	xlt_free(&ctx);

	return EXIT_SUCCESS;
}

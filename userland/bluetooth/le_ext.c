/*
 * le_ext.c (included by btshim.c): bluetoothd's Broadcom LE vendor commands and its legacy LE commands, on the
 * WCN3988's extended LE commands. The WCN answers the legacy advertising/scanning commands (0x200B/0x200C, ...)
 * with 0x0C Command Disallowed; the extended ones work (btshim scan: 35 devices in 15 s).
 *
 * Broadcom multi-advertising (bcm_vsc.c, opcodes found in bluetoothd 20H392):
 *   0xFD48 Create   [addr_type][addr 6, HCI order]          -> reply [status][instance]
 *   0xFD49 Remove   [instance]
 *   0xFD4A Params   [instance][min 2][max 2][a][b][c]        (a = legacy advertising type; b, c logged)
 *   0xFD4B Data     [instance][len][data]
 *   0xFD4C ScanRsp  [instance][len][data]
 *   0xFD4D Enable   [instance][enable][u32][u8]
 *   0xFD4E Address  [instance][addr_type][addr 6]
 * become advertising set `instance` (handle = instance): 0x2036 parameters, 0x2035 set random address, 0x2037
 * data, 0x2038 scan response, 0x2039 enable, 0x203C remove. Legacy advertising (0x2005/0x2006/0x2008/0x2009/
 * 0x200A) uses set 0.
 * Scanning: legacy 0x200B/0x200C and Apple's 0xFE29 (OlympicLESetScanEnable [enable][dup][3 bytes][u16][u16])
 * and 0xFE37 (multi-core parameters, logged) become 0x2041/0x2042; LE Extended Advertising Reports (0x3E/0x0D)
 * go back to bluetoothd as legacy LE Advertising Reports (0x3E/0x02), data cut to 31 bytes.
 * Connections: legacy 0x200D LE Create Connection -> 0x2043 LE Extended Create Connection (its Command Status
 * relabelled), LE Enhanced Connection Complete (0x0A) -> LE Connection Complete (0x01).
 * Our own commands go one at a time (the WCN's command credit is 1): a queue, the next one sent when the
 * previous one's Command Complete/Status arrives; those events never reach bluetoothd (non-zero status logged).
 */

/* ---------------------------------------------------------------- our own command queue */

struct own_cmd { uint16_t op; uint8_t len; uint8_t par[255]; };
static struct own_cmd own_q[48];
static unsigned own_head, own_tail;     /* tail: next to send */
static int own_busy;
static uint16_t own_inflight;
static time_t own_sent_at;

static int
is_own_op(uint16_t op)
{
	return (op >= 0x2035 && op <= 0x203c) || op == 0x2041 || op == 0x2042 || op == 0x2043;
}

static void
own_pump(void)
{
	if (own_busy && time(NULL) - own_sent_at > 2) {
		say("le: no answer to %#06x, going on", own_inflight);
		own_busy = 0;
	}
	if (own_busy || own_tail == own_head)
		return;
	struct own_cmd *c = &own_q[own_tail % 48];
	own_tail++;
	own_busy = 1;
	own_inflight = c->op;
	own_sent_at = time(NULL);
	if (opt_verbose)
		say("le: -> %#06x (%u bytes)", c->op, c->len);
	qca_cmd(c->op, c->par, c->len);
}

static void
own_cmd(uint16_t op, const uint8_t *par, uint8_t len)
{
	if (own_head - own_tail >= 48) {
		say("le: own command queue full, %#06x dropped", op);
		return;
	}
	struct own_cmd *c = &own_q[own_head % 48];
	c->op = op;
	c->len = len;
	memcpy(c->par, par, len);
	own_head++;
	own_pump();
}

/* 1 = this Command Complete/Status answered one of our own commands (not for bluetoothd) */
static int
own_event(const uint8_t *p, size_t n)
{
	uint16_t op;
	uint8_t st;
	if (p[0] == 4 && p[1] == 0x0e && n >= 7) {
		op = (uint16_t)(p[4] | (p[5] << 8));
		st = p[6];
	} else if (p[0] == 4 && p[1] == 0x0f && n >= 7) {
		op = (uint16_t)(p[5] | (p[6] << 8));
		st = p[3];
	} else {
		return 0;
	}
	if (!is_own_op(op))
		return 0;
	if (op == 0x2043 && p[1] == 0x0f) {     /* relabel for bluetoothd, who sent 0x200D */
		uint8_t q[7];
		memcpy(q, p, 7);
		q[5] = 0x0d, q[6] = 0x20;
		to_host(q, 7);
	}
	if (st)
		say("le: WCN answered %#06x with status %#x", op, st);
	if (op == own_inflight) {
		own_busy = 0;
		own_pump();
	}
	return 1;
}

/* ---------------------------------------------------------------- advertising sets */

struct adv_set {
	int used;
	uint8_t addr_type, addr[6];
	uint8_t legacy_type;
	uint16_t min, max;
	int params_sent;
};
static struct adv_set adv[16];          /* index = handle; 0 = legacy advertising */

/* ext event properties from a legacy advertising type */
static uint16_t
adv_props(uint8_t t)
{
	switch (t) {
	case 0: return 0x0013;  /* ADV_IND: legacy, connectable, scannable */
	case 1: return 0x001d;  /* ADV_DIRECT_IND (high duty) */
	case 2: return 0x0012;  /* ADV_SCAN_IND */
	case 3: return 0x0010;  /* ADV_NONCONN_IND */
	case 4: return 0x0015;  /* ADV_DIRECT_IND low duty */
	default: return 0x0013;
	}
}

static void
adv_send_params(int h)
{
	struct adv_set *a = &adv[h];
	uint8_t p[25];
	uint16_t props = adv_props(a->legacy_type);
	uint16_t mn = a->min ? a->min : 0x00a0, mx = a->max ? a->max : 0x00a0;
	p[0] = (uint8_t)h;
	p[1] = props & 0xff, p[2] = props >> 8;
	p[3] = mn & 0xff, p[4] = mn >> 8, p[5] = 0;
	p[6] = mx & 0xff, p[7] = mx >> 8, p[8] = 0;
	p[9] = 0x07;                            /* channels 37, 38, 39 */
	p[10] = a->addr_type ? 0x01 : 0x00;     /* own address: random set by 0x2035, or public */
	p[11] = 0;                              /* peer address type */
	memset(p + 12, 0, 6);                   /* peer address */
	p[18] = 0;                              /* filter policy */
	p[19] = 0x7f;                           /* tx power: no preference */
	p[20] = 0x01;                           /* primary PHY 1M */
	p[21] = 0;                              /* secondary max skip */
	p[22] = 0x01;                           /* secondary PHY 1M */
	p[23] = (uint8_t)(h & 0x0f);            /* SID */
	p[24] = 0;                              /* no scan request notifications */
	own_cmd(0x2036, p, 25);
	if (a->addr_type) {
		uint8_t r[7];
		r[0] = (uint8_t)h;
		memcpy(r + 1, a->addr, 6);
		own_cmd(0x2035, r, 7);
	}
	a->params_sent = 1;
}

static void
adv_data(uint16_t op, int h, const uint8_t *d, unsigned len)
{
	uint8_t p[4 + 251];
	if (len > 251)
		len = 251;
	if (!adv[h].params_sent)
		adv_send_params(h);
	p[0] = (uint8_t)h, p[1] = 0x03, p[2] = 0x01, p[3] = (uint8_t)len;     /* complete data, no fragmentation */
	memcpy(p + 4, d, len);
	own_cmd(op, p, (uint8_t)(4 + len));
}

static void
adv_enable(int h, int on)
{
	uint8_t p[6] = { (uint8_t)(on ? 1 : 0), 1, (uint8_t)h, 0, 0, 0 };
	if (on && !adv[h].params_sent)
		adv_send_params(h);
	own_cmd(0x2039, p, 6);
}

/* ---------------------------------------------------------------- scanning */

static uint8_t scan_type = 1;           /* active */
static uint16_t scan_int = 0x0030, scan_win = 0x0030;
static uint8_t scan_own = 0, scan_filter = 0;
static int le_scanning;

static void
scan_apply(int on, uint8_t dup)
{
	if (on) {
		uint8_t p[8] = { scan_own, scan_filter, 0x01, scan_type,
			(uint8_t)(scan_int & 0xff), (uint8_t)(scan_int >> 8), (uint8_t)(scan_win & 0xff), (uint8_t)(scan_win >> 8) };
		if (le_scanning) {
			uint8_t off[6] = { 0 };
			own_cmd(0x2042, off, 6);
		}
		own_cmd(0x2041, p, 8);
	}
	uint8_t e[6] = { (uint8_t)(on ? 1 : 0), dup, 0, 0, 0, 0 };
	own_cmd(0x2042, e, 6);
	le_scanning = on;
	say("le: scanning %s (type %u, interval %#x, window %#x)", on ? "on" : "off", scan_type, scan_int, scan_win);
}

/* ---------------------------------------------------------------- bluetoothd's commands */

static uint8_t
ok_reply(uint16_t op)
{
	uint8_t z = 0;
	host_cc(op, &z, 1);
	return 0;
}

/* Broadcom LE VSCs; 1 = handled (bluetoothd answered) */
static int
le_vsc(uint16_t op, const uint8_t *par, size_t plen)
{
	int h = plen ? par[0] : 0;
	if (h < 0 || h >= 16)
		h = 15;
	switch (op) {
	case 0xfd48: {          /* create: [type][addr6] -> [status][instance] */
		int n;
		for (n = 1; n < 16 && adv[n].used; n++)
			;
		if (n == 16) {
			uint8_t r[2] = { 0x07, 0 };     /* memory capacity exceeded */
			host_cc(op, r, 2);
			return 1;
		}
		memset(&adv[n], 0, sizeof(adv[n]));
		adv[n].used = 1;
		if (plen >= 7) {
			adv[n].addr_type = par[0];
			memcpy(adv[n].addr, par + 1, 6);
		}
		uint8_t r[2] = { 0, (uint8_t)n };
		host_cc(op, r, 2);
		say("le: multi-adv instance %d created (address type %u)", n, adv[n].addr_type);
		return 1;
	}
	case 0xfd49:            /* remove [instance] */
		if (h > 0 && adv[h].used) {
			uint8_t p[1] = { (uint8_t)h };
			if (adv[h].params_sent)
				own_cmd(0x203c, p, 1);
			adv[h].used = 0;
		}
		ok_reply(op);
		return 1;
	case 0xfd4a:            /* params [instance][min2][max2][a][b][c] */
		if (plen >= 8) {
			adv[h].min = (uint16_t)(par[1] | (par[2] << 8));
			adv[h].max = (uint16_t)(par[3] | (par[4] << 8));
			adv[h].legacy_type = par[5];
			say("le: instance %d params min %#x max %#x type %u, b %#x c %#x", h, adv[h].min, adv[h].max,
			    par[5], par[6], par[7]);
			adv_send_params(h);
		}
		ok_reply(op);
		return 1;
	case 0xfd4b:            /* data [instance][len][data] */
	case 0xfd4c:            /* scan response */
		if (plen >= 2) {
			unsigned len = par[1];
			if (len > plen - 2)
				len = (unsigned)(plen - 2);
			adv_data(op == 0xfd4b ? 0x2037 : 0x2038, h, par + 2, len);
		}
		ok_reply(op);
		return 1;
	case 0xfd4d:            /* enable [instance][enable][u32][u8] */
		if (plen >= 2) {
			say("le: instance %d advertising %s", h, par[1] ? "on" : "off");
			adv_enable(h, par[1]);
		}
		ok_reply(op);
		return 1;
	case 0xfd4e:            /* address [instance][type][addr6] */
		if (plen >= 8) {
			adv[h].addr_type = par[1];
			memcpy(adv[h].addr, par + 2, 6);
			if (adv[h].params_sent && par[1]) {
				uint8_t r[7];
				r[0] = (uint8_t)h;
				memcpy(r + 1, par + 2, 6);
				own_cmd(0x2035, r, 7);
			}
		}
		ok_reply(op);
		return 1;
	case 0xfe29: {          /* Olympic scan enable [enable][dup][x][y][z][u16][u16] */
		char hex[64];
		int k = 0;
		for (size_t i = 0; i < plen && i < 16; i++)
			k += snprintf(hex + k, sizeof(hex) - (size_t)k, "%02x ", par[i]);
		say("le: Olympic scan enable: %s", hex);
		if (plen >= 1)
			scan_apply(par[0] != 0, plen >= 2 ? par[1] : 0);
		ok_reply(op);
		return 1;
	}
	case 0xfe37: {          /* Olympic multi-core scan parameters: logged; extended parameters stay ours */
		char hex[160];
		int k = 0;
		for (size_t i = 0; i < plen && i < 48; i++)
			k += snprintf(hex + k, sizeof(hex) - (size_t)k, "%02x ", par[i]);
		say("le: Olympic multi-core scan parameters (%zu bytes): %s", plen, hex);
		ok_reply(op);
		return 1;
	}
	default:
		return 0;
	}
}

/* legacy LE commands the WCN disallows; 1 = handled */
static int
le_legacy(uint16_t op, const uint8_t *par, size_t plen)
{
	switch (op) {
	case 0x2005:            /* LE Set Random Address: still valid for scanning; also set 0's address */
		if (plen >= 6) {
			adv[0].addr_type = 1;
			memcpy(adv[0].addr, par, 6);
		}
		return 0;       /* forwarded as it is */
	case 0x2006:            /* LE Set Advertising Parameters [min2][max2][type][own][peer type][peer6][map][filter] */
		if (plen >= 15) {
			adv[0].used = 1;
			adv[0].min = (uint16_t)(par[0] | (par[1] << 8));
			adv[0].max = (uint16_t)(par[2] | (par[3] << 8));
			adv[0].legacy_type = par[4];
			if (par[5] == 0)
				adv[0].addr_type = 0;
			adv_send_params(0);
		}
		ok_reply(op);
		return 1;
	case 0x2008:            /* LE Set Advertising Data [len][31] */
	case 0x2009:            /* LE Set Scan Response Data */
		if (plen >= 1) {
			unsigned len = par[0];
			if (len > plen - 1)
				len = (unsigned)(plen - 1);
			adv_data(op == 0x2008 ? 0x2037 : 0x2038, 0, par + 1, len);
		}
		ok_reply(op);
		return 1;
	case 0x200a:            /* LE Set Advertising Enable [enable] */
		if (plen >= 1)
			adv_enable(0, par[0]);
		ok_reply(op);
		return 1;
	case 0x200b:            /* LE Set Scan Parameters [type][int2][win2][own][filter] */
		if (plen >= 7) {
			scan_type = par[0];
			scan_int = (uint16_t)(par[1] | (par[2] << 8));
			scan_win = (uint16_t)(par[3] | (par[4] << 8));
			scan_own = par[5];
			scan_filter = par[6];
		}
		ok_reply(op);
		return 1;
	case 0x200c:            /* LE Set Scan Enable [enable][dup] */
		if (plen >= 2)
			scan_apply(par[0] != 0, par[1]);
		ok_reply(op);
		return 1;
	case 0x200d:            /* LE Create Connection -> LE Extended Create Connection (1M) */
		if (plen >= 25) {
			uint8_t p[26];
			p[0] = par[4];                  /* initiator filter policy */
			p[1] = par[12];                 /* own address type */
			p[2] = par[5];                  /* peer address type */
			memcpy(p + 3, par + 6, 6);      /* peer address */
			p[9] = 0x01;                    /* 1M */
			memcpy(p + 10, par + 0, 4);     /* scan interval, window */
			memcpy(p + 14, par + 13, 12);   /* conn interval min/max, latency, timeout, CE min/max */
			own_cmd(0x2043, p, 26);         /* its Command Status is relabelled 0x200D in own_event */
			say("le: create connection -> 0x2043");
		} else {
			host_cs(op, 0x12);
		}
		return 1;
	case 0x200e:            /* LE Create Connection Cancel: valid for both kinds, forwarded */
	default:
		return 0;
	}
}

/* ---------------------------------------------------------------- events back to bluetoothd */

/* 1 = consumed (converted or dropped) */
static int
le_event(const uint8_t *p, size_t n)
{
	if (p[0] != 4 || p[1] != 0x3e || n < 4)
		return 0;
	uint8_t sub = p[3];
	if (sub == 0x0d && n >= 5) {            /* LE Extended Advertising Report -> LE Advertising Report */
		unsigned nr = p[4];
		const uint8_t *r = p + 5;
		for (unsigned i = 0; i < nr && r + 24 <= p + n; i++) {
			uint16_t et = (uint16_t)(r[0] | (r[1] << 8));
			unsigned dl = r[23];
			uint8_t lt;
			if (et & 0x10) {                /* legacy PDU */
				if (et & 0x08)
					lt = 4;                 /* scan response */
				else if ((et & 0x07) == 0x03)
					lt = 0;                 /* ADV_IND */
				else if ((et & 0x07) == 0x05)
					lt = 1;                 /* ADV_DIRECT_IND */
				else if ((et & 0x07) == 0x02)
					lt = 2;                 /* ADV_SCAN_IND */
				else
					lt = 3;                 /* ADV_NONCONN_IND */
			} else {
				lt = (et & 0x08) ? 4 : (et & 0x01) ? 0 : (et & 0x02) ? 2 : 3;
			}
			unsigned cl = dl > 31 ? 31 : dl;
			uint8_t q[3 + 2 + 1 + 1 + 1 + 6 + 1 + 31 + 1];
			size_t k = 0;
			q[k++] = 4, q[k++] = 0x3e, q[k++] = 0;
			q[k++] = 0x02, q[k++] = 1;
			q[k++] = lt;
			q[k++] = r[2] & 0x01;           /* address type (identity types 2/3 -> 0/1) */
			memcpy(q + k, r + 3, 6);
			k += 6;
			q[k++] = (uint8_t)cl;
			memcpy(q + k, r + 24, cl);
			k += cl;
			q[k++] = r[13];                 /* RSSI */
			q[2] = (uint8_t)(k - 3);
			to_host(q, k);
			r += 24 + dl;
		}
		return 1;
	}
	if (sub == 0x0a && n >= 34) {           /* LE Enhanced Connection Complete -> LE Connection Complete */
		uint8_t q[3 + 19];
		q[0] = 4, q[1] = 0x3e, q[2] = 19;
		q[3] = 0x01;
		memcpy(q + 4, p + 4, 4);                /* status, handle, role */
		q[8] = p[8] & 0x01;                     /* peer address type */
		memcpy(q + 9, p + 9, 6);                /* peer address */
		memcpy(q + 15, p + 27, 7);              /* interval, latency, timeout, clock accuracy */
		to_host(q, sizeof(q));
		say("le: connection complete status %#x handle %#x", p[4], p[5] | (p[6] << 8));
		return 1;
	}
	if (sub == 0x12)                        /* advertising set terminated: ours */
		return 1;
	return 0;
}

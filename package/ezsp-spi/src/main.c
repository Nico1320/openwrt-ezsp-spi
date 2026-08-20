#include "ezsp-spi.h"

void usage(void)
{
	printf(
"usage: ezsp-spi <probe|info|status|config|policy|scan|join|run|bridge|\n"
"       leave> [options]\n"
"\n"
"  probe   reset the NCP and run the EZSP-SPI handshake\n"
"  info    read EUI64, node id and network state (read-only)\n"
"  status  query the running daemon over its control socket\n"
"  config  dump the NCP's EZSP configuration values (read-only)\n"
"  policy  dump the NCP's EZSP policies (read-only)\n"
"  keys    report which security keys this node holds (read-only)\n"
"  neighbors  show neighbour table, children and routes (read-only)\n"
"  permit [s] open this node's join window (default 180 s)\n"
"  power [dbm] set transmit power on the live radio\n"
"  bootloader report the standalone bootloader (read-only)\n"
"  verify <ebl> check an image is built for this chip before flashing it\n"
"  scan    active scan: list networks the radio can hear (read-only)\n"
"  join    join an existing network as a router (needs --channel and\n"
"          --pan-id; open permit-join on the coordinator first)\n"
"  run     restore the joined network from NVM and stay up\n"
"  bridge  serve the radio over TCP as an ASH endpoint, so zigbee2mqtt\n"
"          or bellows can drive it directly, coordinator included\n"
"  leave   leave the network and clear the stored state (undoes join)\n"
"\n"
"options:\n"
"  --spidev PATH    spi device            (default %s)\n"
"  --chip PATH      gpio chip             (default %s)\n"
"  --reset N        nRESET line           (default %u)\n"
"  --wake N         nWAKE line            (default %u)\n"
"  --int N          nHOST_INT line        (default %u)\n"
"  --nssel-int N    nSSEL_INT line, autodetected from the board\n"
"                   (V2 gpio%u, V1 gpio56). AN711 requires this to carry\n"
"                   the same signal as nSSEL; it is not a second chip\n"
"                   select. On the V1 gpio29 is the eMMC's instead.\n"
"  --nssel-int-mode M  follow | low | high | input   (default %s).\n"
"                   \"follow\" drives it with nSSEL, which is what the\n"
"                   part expects and what lets it sleep.\n"
"  --speed HZ       spi clock             (default %u, vendor uses 12000000)\n"
"  --gap-us N       idle time between transactions (default %u)\n"
"  --no-reset       do not reset the NCP first\n"
"  --no-wake        skip the nWAKE handshake before each transaction\n"
"  --wait-bytes N   idle bytes to clock waiting for a reply (default %u)\n"
"  --ezsp-version N EZSP version to request first (default 4). Set it to\n"
"                   the NCP's own version to skip a renegotiation reset.\n"
"\n"
"bridge options:\n"
"  --port N         tcp port to listen on (default %u)\n"
"  --bind ADDR      address to listen on  (default %s)\n"
"                   zigbee2mqtt: adapter ezsp, port tcp://<host>:%u\n"
"\n"
"join options:\n"
"  --channel N            radio channel\n"
"  --pan-id 0xNNNN        pan id\n"
"  --ext-pan-id X         8 bytes, hex with ':' or decimal with ','\n"
"  --reverse-ext-pan-id   flip the byte order (try this if the join fails)\n"
"  --tx-power N           radio tx power in dBm (default 8)\n"
"  --sec-bitmask 0xNNNN   EmberInitialSecurityBitmask (default 0x0104)\n"
"  --join-timeout N       seconds to wait for NETWORK_UP (default 60)\n"
"  --join-retries N       retries on a transient join failure (default 2)\n"
"  --report-secs N        seconds between attribute reports, 0 = off\n"
"                         (default 300; feeds linkquality)\n"
"  --no-precheck          skip the pre-join scan (then --ext-pan-id is\n"
"                         required and byte order is on you)\n"
"  -v               hexdump every transfer\n",
	cfg.spidev, cfg.gpiochip, cfg.reset_line, cfg.wake_line, cfg.int_line,
	cfg.nssel_int_line, cfg.nssel_int_mode, cfg.speed, cfg.gap_us,
	cfg.wait_bytes, cfg.bridge_port, cfg.bind_addr,
	cfg.bridge_port);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "spidev",	required_argument, NULL, 1 },
		{ "chip",	required_argument, NULL, 2 },
		{ "reset",	required_argument, NULL, 3 },
		{ "wake",	required_argument, NULL, 4 },
		{ "int",	required_argument, NULL, 5 },
		{ "nssel-int",	required_argument, NULL, 7 },
		{ "nssel-int-mode", required_argument, NULL, 8 },
		{ "aux",	required_argument, NULL, 7 },
		{ "aux-mode",	required_argument, NULL, 8 },
		// Former names. gpio29 was called "nCS" after the vendor
		// binary's pin table, which is wrong -- kept so configs
		// written against the old flags still work.
		{ "ncs",	required_argument, NULL, 7 },
		{ "ncs-mode",	required_argument, NULL, 8 },
		{ "speed",	required_argument, NULL, 9 },
		{ "gap-us",	required_argument, NULL, 13 },
		{ "no-reset",	no_argument,	   NULL, 14 },
		{ "no-wake",	no_argument,	   NULL, 16 },
		{ "wait-bytes",	required_argument, NULL, 19 },
		{ "channel",	required_argument, NULL, 20 },
		{ "pan-id",	required_argument, NULL, 21 },
		{ "ext-pan-id",	required_argument, NULL, 22 },
		{ "reverse-ext-pan-id", no_argument, NULL, 23 },
		{ "tx-power",	required_argument, NULL, 24 },
		{ "sec-bitmask", required_argument, NULL, 25 },
		{ "join-timeout", required_argument, NULL, 26 },
		{ "join-retries", required_argument, NULL, 29 },
		{ "report-secs", required_argument, NULL, 30 },
		{ "no-precheck", no_argument,	   NULL, 27 },
		{ "ezsp-version", required_argument, NULL, 31 },
		{ "port",	required_argument, NULL, 33 },
		{ "bind",	required_argument, NULL, 34 },
		{ "socket",	required_argument, NULL, 28 },
		{ "help",	no_argument,	   NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	const char *cmd, *pos;
	int c;

	if (argc < 2) {
		usage();
		return 1;
	}
	cmd = argv[1];
	current_cmd = cmd;
	optind = 2;

	while ((c = getopt_long(argc, argv, "vh", opts, NULL)) != -1) {
		switch (c) {
		case 33: cfg.bridge_port = (unsigned)strtoul(optarg, NULL, 0);
			 break;
		case 34: cfg.bind_addr = optarg; break;
		case 1:  cfg.spidev = optarg; break;
		case 2:  cfg.gpiochip = optarg; break;
		case 3:  cfg.reset_line = strtoul(optarg, NULL, 0);
			 cfg.reset_set = 1; break;
		case 4:  cfg.wake_line = strtoul(optarg, NULL, 0);
			 cfg.wake_set = 1; break;
		case 5:  cfg.int_line = strtoul(optarg, NULL, 0);
			 cfg.int_set = 1; break;
		case 7:  cfg.nssel_int_line = strtoul(optarg, NULL, 0);
			 cfg.nssel_int_set = 1; break;
		case 8:  cfg.nssel_int_mode = optarg; break;
		case 9:  cfg.speed = strtoul(optarg, NULL, 0); break;
		case 13: cfg.gap_us = strtoul(optarg, NULL, 0); break;
		case 14: cfg.no_reset = 1; break;
		case 16: cfg.wake_handshake = 0; break;
		case 19: cfg.wait_bytes = strtoul(optarg, NULL, 0); break;
		case 20: cfg.channel = strtoul(optarg, NULL, 0); break;
		case 21:
			cfg.pan_id = (uint16_t)strtoul(optarg, NULL, 0);
			cfg.have_pan_id = 1;
			break;
		case 22:
			parse_ext_pan_id(optarg);
			cfg.have_ext_pan_id = 1;
			break;
		case 23: reverse_ext_pan_id = 1; break;
		case 24: cfg.tx_power = (int)strtol(optarg, NULL, 0); break;
		case 25:
			cfg.sec_bitmask = (uint16_t)strtoul(optarg, NULL, 0);
			break;
		case 26: cfg.join_timeout = strtoul(optarg, NULL, 0); break;
		case 29: cfg.join_retries = strtoul(optarg, NULL, 0); break;
		case 30: cfg.report_secs = strtoul(optarg, NULL, 0); break;
		case 27: cfg.no_precheck = 1; break;
		case 31: cfg.ezsp_want = strtoul(optarg, NULL, 0); break;
		case 28: cfg.sock_path = optarg; break;
		case 'v': cfg.verbose = 1; break;
		case 'h': usage(); return 0;
		default: usage(); return 1;
		}
	}

	// getopt_long permutes argv, so positionals are not at argv[2].
	pos = optind < argc ? argv[optind] : NULL;

	if (!strcmp(cmd, "probe"))
		return cmd_probe();
	if (reverse_ext_pan_id) {
		uint8_t t;
		int a, b;

		for (a = 0, b = 7; a < b; a++, b--) {
			t = cfg.ext_pan_id[a];
			cfg.ext_pan_id[a] = cfg.ext_pan_id[b];
			cfg.ext_pan_id[b] = t;
		}
	}

	if (!strcmp(cmd, "info"))
		return cmd_info();
	if (!strcmp(cmd, "status"))
		return cmd_status();
	if (!strcmp(cmd, "config"))
		return cmd_config();
	if (!strcmp(cmd, "policy"))
		return cmd_policy();
	if (!strcmp(cmd, "keys"))
		return cmd_keys();
	if (!strcmp(cmd, "neighbors"))
		return cmd_neighbors();
	if (!strcmp(cmd, "permit"))
		return cmd_permit(pos);
	if (!strcmp(cmd, "power"))
		return cmd_power(pos);
	if (!strcmp(cmd, "bootloader"))
		return cmd_bootloader();
	if (!strcmp(cmd, "verify"))
		return cmd_verify(pos);
	if (!strcmp(cmd, "scan"))
		return cmd_scan();
	if (!strcmp(cmd, "join"))
		return cmd_join();
	if (!strcmp(cmd, "bridge"))
		return cmd_bridge();
	if (!strcmp(cmd, "run"))
		return cmd_run();
	if (!strcmp(cmd, "leave"))
		return cmd_leave();

	usage();
	return 1;
}

/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#define _GNU_SOURCE
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <netinet/ether.h>
#include <time.h>

#include <jansson.h>

#include <ayla/utypes.h>
#include <ayla/file_io.h>
#include <ayla/conf_io.h>
#include <ayla/log.h>
#include <ayla/base64.h>
#include <ayla/hex.h>
#include <ayla/build.h>
#include <ayla/crypto.h>
#include "xml.h"

#define DEVICE_REGION_DEFAULT	"US"
#define DEVICE_MODEL_DEFAULT	"AY001MRT1"
#define MFG_SERIAL_DEFAULT	""
#define MFG_SW_VERSION_DEFAULT	""
#define ODM_DEFAULT		""
#define CONFIG_GEN_BANNER	"config_gen " BUILD_VERSION_LABEL

#define MFG_LOG_VERSION		3	/* Log format to use */

bool no_mfg_log;
static const char *dsn_path;
static const char *oem_info_path;
static const char *mac_address;
static const char *output_dir = "/etc/ayla";
static char verbosity = '1';

struct device_info {
	char *dsn;		/* Ayla Device Serial Number */
	char *pub_key;		/* RSA public key for device */
	char *region;		/* Region code indicating the service to use */
};
struct oem_info {
	char *oem;		/* OEM ID */
	char *oem_model;	/* OEM model (for template association) */
	char *oem_secret;	/* OEM secret (used to generate OEM key) */
};
struct mfg_info {
	char *mfg_model;	/* Hardware model */
	char *mfg_serial;	/* Hardware serial number (optional) */
	char *mfg_sw_version;	/* SW version installed in factory (optional) */
	char *odm;		/* Manufacturer of complete device (optional) */
};

static struct device_info device_info;
static struct oem_info oem_info;
static struct mfg_info mfg_info;
static const char *cmdname;


static void usage(void)
{
	fprintf(stderr,
	    "%s\n"
	    "Usage: %s -d <dsn_path> -i <oem_info_file> [OPTIONS]\n"
	    "  REQUIRED:\n"
	    "    -d <dsn_path>           Path to DSN .xml file from AFS\n"
	    "    -i <oem_info_file>      Path to file with OEM info (see ex.)\n"
	    "  OPTIONS:\n"
	    "    -n                      Omit mfg log file generation\n"
	    "    -m <MAC_address>        Device MAC addr (omit when using -n)\n"
	    "    -o <output_dir>         Directory to create for output files\n"
	    "    -v <verbosity>          0 = silent, 1 = default, 2 = verbose\n"
	    "  Examples:\n"
	    "    %s -d dsns/AC000W000123456.xml "
	    "-i ./oem_info -m 112233445566 -o ./ayla_config -v 2\n"
	    "    %s -n -d dsns/AC000W000123457.xml -i ./oem_info\n",
	    CONFIG_GEN_BANNER, cmdname, cmdname, cmdname);
	exit(1);
}

/*
 * Return OEM key
 * Return value is malloced.
 */
char *gen_make_oem_key(const char *secret,
    const char *oem,
    const char *oem_model,
    const char *pub_key)
{
	struct crypto_state rsa = { 0 };
	time_t t;
	char input_buf[1024];
	char rsa_buf[1024];
	size_t len;
	ssize_t rsa_len;
	char *output;

	time(&t);

	/* compose the raw data string */
	len = snprintf(input_buf, sizeof(input_buf), "%u %s %s %s\n",
	    (unsigned)t, secret, oem, oem_model);
	if (len >= sizeof(input_buf)) {
		log_err("%zu byte input buffer is too small",
		    sizeof(input_buf));
		return NULL;
	}

	/* encrypt the data using RSA */
	if (crypto_init_rsa(&rsa, RSA_KEY_PUBLIC, pub_key) < 0) {
		return NULL;
	}
	rsa_len = crypto_encrypt(&rsa, input_buf, strlen(input_buf),
	    rsa_buf, sizeof(rsa_buf));
	crypto_cleanup(&rsa);
	if (rsa_len <= 0) {
		log_err("oem key encryption failed");
		return NULL;
	}

	/* encode the OEM key in base64 */
	output = base64_encode(rsa_buf, rsa_len, NULL);
	if (!output) {
		log_err("base64 encoding failed");
		return NULL;
	}
	return output;
}

/*
 * XML parser callback for dsn tag
 */
static int gen_parse_dsn(struct xml_state *sp, int argc, char **argv)
{
	if (argc < 1) {
		log_err("missing XML value for \"dsn\"");
		return -1;
	}
	device_info.dsn = strdup(argv[0]);
	log_debug("DSN: %s", device_info.dsn);
	return 0;
}

/*
 * XML parser callback for public-key tag
 */
static int gen_parse_pubkey(struct xml_state *sp, int argc, char **argv)
{
	if (argc < 1) {
		log_err("missing XML value for \"public-key\"");
		return -1;
	}
	device_info.pub_key = strdup(argv[0]);
	log_debug("pub_key: %s", device_info.pub_key);
	return 0;
}

/*
 * XML parser tag definitions
 */
static const struct xml_tag dsn_tags[] = {
	XML_TAG("dsn", NULL, gen_parse_dsn),
	XML_TAG_WS("public-key", NULL, gen_parse_pubkey),
	XML_TAG(NULL, NULL, NULL)
};
static const struct xml_tag dsn_top[] = {
	XML_TAG("f-device", dsn_tags, NULL),
	XML_TAG(NULL, NULL, NULL)
};

/*
 * Parse a DSN file from AFS (XML)
 */
static int gen_parse_dsn_file(void)
{
	FILE *fp;
	size_t size;
	int ret = -1;
	char buf[100];
	struct xml_state state;

	fp = fopen(dsn_path, "r");
	if (!fp) {
		log_err("failed to open %s: %m", dsn_path);
		return -1;
	}

	xml_parse_init(&state, dsn_top);

	while ((size = fread(buf, 1, sizeof(buf), fp)) > 0) {
		xml_parse(&state, buf, size);
		if (state.state == XS_DONE) {
			log_debug("parsing %s successful", dsn_path);
			ret = 0;
			break;
		} else if (state.state == XS_ERROR) {
			log_err("parsing %s failed", dsn_path);
			break;
		}
	}
	fclose(fp);
	return ret;
}

/*
 * Read config file and find requested key-value pair.
 * Returns malloc'd value string, or NULL on failure.
 */
static char *gen_read_config_token(const char *path, const char *name)
{
	FILE *fp;
	char buf[200];
	char *token;
	char *value = NULL;
	char *cp;

	fp = fopen(path, "r");
	if (!fp) {
		log_err("failed to open %s: %m", path);
		return NULL;
	}
	while (fgets(buf, sizeof(buf), fp)) {
		cp = strpbrk(buf, "\n#");
		if (cp) {
			*cp = '\0';	/* end line at newline or comment */
		}
		token = strtok(buf, " =:\t");
		if (!token || strcmp(name, token) != 0) {
			continue;
		}
		value = strtok(NULL, " \t");
		if (!value) {
			log_err("missing value for %s", token);
			break;
		}
		break;
	}
	fclose(fp);

	if (!value) {
		return NULL;
	}
	log_debug("%s = %s", token, value);
	return strdup(value);
}

/*
 * Load required values from the OEM info file
 */
static int gen_parse_oem_info(void)
{
	device_info.region = gen_read_config_token(oem_info_path, "region");
	if (!device_info.region) {
		device_info.region =  strdup(DEVICE_REGION_DEFAULT);
		log_debug("no region defined: defaulting to \"%s\"",
		    device_info.region);
	}
	oem_info.oem = gen_read_config_token(oem_info_path, "oem");
	if (!oem_info.oem) {
		log_err("missing \"oem\"");
		return -1;
	}
	oem_info.oem_model = gen_read_config_token(oem_info_path, "oem_model");
	if (!oem_info.oem_model) {
		log_err("missing \"oem_model\"");
		return -1;
	}
	oem_info.oem_secret = gen_read_config_token(oem_info_path,
	    "oem_secret");
	if (!oem_info.oem_secret) {
		log_err("missing \"oem_secret\"");
		return -1;
	}
	mfg_info.mfg_model = gen_read_config_token(oem_info_path, "mfg_model");
	if (!mfg_info.mfg_model) {
		log_err("missing \"mfg_model\"");
		return -1;
	}
	mfg_info.mfg_serial = gen_read_config_token(oem_info_path,
	    "mfg_serial");
	if (!mfg_info.mfg_serial) {
		mfg_info.mfg_serial =  strdup(MFG_SERIAL_DEFAULT);
		log_debug("no mfg_serial defined: defaulting to \"%s\"",
		    mfg_info.mfg_serial);
	}
	mfg_info.mfg_sw_version = gen_read_config_token(oem_info_path,
	    "mfg_sw_version");
	if (!mfg_info.mfg_sw_version) {
		mfg_info.mfg_sw_version =  strdup(MFG_SW_VERSION_DEFAULT);
		log_debug("no mfg_sw_version defined: defaulting to \"%s\"",
		    mfg_info.mfg_sw_version);
	}
	mfg_info.odm = gen_read_config_token(oem_info_path, "odm");
	if (!mfg_info.odm) {
		mfg_info.odm =  strdup(ODM_DEFAULT);
		log_debug("no odm defined: defaulting to \"%s\"",
		    mfg_info.odm);
	}

	return 0;
}

/*
 * Set required config values in the config JSON structure
 */
static int gen_populate_config(void)
{
	char *oem_key;

	if (conf_set_new("sys/factory", json_integer(1)) < 0) {
		log_err("failed to set: sys/factory");
		return -1;
	}
	if (conf_set_new("id/dsn", json_string(device_info.dsn)) < 0) {
		log_err("failed to set: id/dsn");
		return -1;
	}
	if (conf_set_new("id/rsa_pub_key",
	    json_string(device_info.pub_key)) < 0) {
		log_err("failed to set: id/rsa_pub_key");
		return -1;
	}
	if (conf_set_new("client/region",
	    json_string(device_info.region)) < 0) {
		log_err("failed to set: client/region");
		return -1;
	}
	if (conf_set_new("oem/oem", json_string(oem_info.oem)) < 0) {
		log_err("failed to set: oem/oem");
		return -1;
	}
	if (conf_set_new("oem/model", json_string(oem_info.oem_model)) < 0) {
		log_err("failed to set: oem/model");
		return -1;
	}
	oem_key = gen_make_oem_key(oem_info.oem_secret, oem_info.oem,
	    oem_info.oem_model, device_info.pub_key);
	if (!oem_key) {
		log_err("failed generating OEM key");
		return -1;
	}
	if (conf_set_new("oem/key", json_string(oem_key)) < 0) {
		log_err("failed to set: oem/key");
		free(oem_key);
		return -1;
	}
	free(oem_key);
	return 0;
}

/*
 * Generate a log entry for the current device configuration.
 * Returns a malloc'd string in CSV format with a trailing newline.
 *
 * FIELDS (v2):
 *   version
 *   time
 *   time_string
 *   status {pass,fail,label,config,config_test,verify,assign}
 *   err_code
 *   device_model
 *   dsn
 *   mac
 *   mfg_model
 *   mfg_sn
 *   hwsig
 *   extra comment
 *
 * ADDITIONAL FIELDS (v3)
 *   oem_id
 *   oem_model
 *   ads_connect_flag {0,1,}
 *   odm
 *   mfg_sw_version
 *
 * EXAMPLE:
 *   3,1427496821,2015/03/27 22:53:41 UTC,label,0,AY001MRT1,AC000W000123456,
 *       112233445566,my-mfg-model,my-mfg-serial,MAC-112233445566,a comment,
 *       aabbccee,test_oem_model,,,
 */
static char *gen_create_log_entry(int log_version)
{
	ssize_t size;
	char *line;
	time_t t;
	struct tm *tm;

	time(&t);
	tm = gmtime(&t);

	switch (log_version) {
	case 2:
		size = asprintf(&line,
		    "%d,%u,%04d/%02d/%02d %02d:%02d:%02d UTC,"
		    "label,0,%s,%s,%s,%s,%s,MAC-%s,Generated by %s\n",
		    log_version,
		    (unsigned)t,
		    tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
		    tm->tm_hour, tm->tm_min, tm->tm_sec,
		    DEVICE_MODEL_DEFAULT, device_info.dsn, mac_address,
		    mfg_info.mfg_model, mfg_info.mfg_serial, mac_address,
		    cmdname);
		break;
	case 3:
		size = asprintf(&line,
		    "%d,%u,%04d/%02d/%02d %02d:%02d:%02d UTC,"
		    "label,0,%s,%s,%s,%s,%s,MAC-%s,Generated by %s,"
		    "%s,%s,,%s,%s\n",
		    log_version,
		    (unsigned)t,
		    tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
		    tm->tm_hour, tm->tm_min, tm->tm_sec,
		    DEVICE_MODEL_DEFAULT, device_info.dsn, mac_address,
		    mfg_info.mfg_model, mfg_info.mfg_serial, mac_address,
		    cmdname, oem_info.oem, oem_info.oem_model, mfg_info.odm,
		    mfg_info.mfg_sw_version);
		break;
	default:
		log_err("unsupported log format: %d", log_version);
		return NULL;
	}
	if (size < 0) {
		log_err("error creating log entry");
		return NULL;

	}
	return line;
}

/*
 * If a log file exists, scan it for duplicate entries.
 * Return 0 if no duplicates found, or -1 if this run is a duplicate.
 */
static int gen_validate_log_file(const char *path, int log_version)
{
	FILE *fp;
	char buf[200];
	char *dsn;
	char *mac;
	int duplicate_found = 0;

	/* XXX support other log versions as needed */
	if (log_version < 2 || log_version > 3) {
		log_err("log version %d not supported", log_version);
		return -1;
	}

	fp = fopen(path, "r");
	if (!fp) {
		if (errno == ENOENT) {
			log_debug("no log file %s to validate", path);
			return 0;
		}
		log_err("failed to open %s: %m", path);
		return -1;
	}
	while (fgets(buf, sizeof(buf), fp)) {
		/* skip unneeded tokens */
		if (!strtok(buf, ",") ||	/* version */
		    !strtok(NULL, ",") ||	/* time */
		    !strtok(NULL, ",") ||	/* time str */
		    !strtok(NULL, ",") ||	/* status */
		    !strtok(NULL, ",") ||	/* err */
		    !strtok(NULL, ",")) {	/* model */
			log_debug("invalid line: skipping");
			continue;
		}
		dsn = strtok(NULL, ",");
		if (!dsn) {
			log_debug("missing DSN: skipping");
			continue;
		}
		mac = strtok(NULL, ",");
		if (!mac) {
			log_debug("missing MAC: skipping");
			continue;
		}
		if (!strcmp(device_info.dsn, dsn)) {
			log_warn("config with duplicate DSN found: %s", dsn);
			duplicate_found = 1;
			break;
		}
		if (!strcmp(mac_address, mac)) {
			log_warn("config with duplicate MAC found: %s", mac);
			duplicate_found = 1;
			break;
		}
	}
	fclose(fp);
	return duplicate_found ? -1 : 0;
}

/*
 * Add a new line to the log file.
 */
static int gen_append_log_entry(const char *path, int log_version)
{
	FILE *fp = NULL;
	int ret = -1;
	char *line = NULL;

	fp = fopen(path, "a+");
	if (!fp) {
		log_err("failed to open %s: %m", path);
		return -1;
	}
	line = gen_create_log_entry(log_version);
	if (!line) {
		goto error;
	}

	if (fputs(line, fp) == EOF) {
		log_err("error writing log entry");
		goto error;
	}
	log_debug("%s", line);
	ret = 0;
error:
	free(line);
	if (fp) {
		fclose(fp);
	}
	return ret;
}

/*
 * Read command line options.
 */
static void gen_opts(int argc, char **argv)
{
	int opt;
	u8 mac_bytes[ETH_ALEN];
	char delim;

	cmdname = strrchr(argv[0], '/');
	if (cmdname) {
		cmdname++;
	} else {
		cmdname = argv[0];
	}

	while ((opt = getopt(argc, argv, "nd:i:t:m:o:v:")) != -1) {
		switch (opt) {
		case 'n':
			no_mfg_log = true;
			break;
		case 'd':
			dsn_path = optarg;
			break;
		case 'i':
			oem_info_path = optarg;
			break;
		case 't':
			/* DEPRECATED OPTION */
			break;
		case 'm':
			mac_address = optarg;
			break;
		case 'o':
			output_dir = optarg;
			break;
		case 'v':
			verbosity = optarg[0];
			break;
		default:
			usage();
			break;
		}
	}

	if (!dsn_path) {
		fprintf(stderr, "Missing dsn_path\n");
		usage();
	}
	if (!oem_info_path) {
		fprintf(stderr, "Missing oem_info_path\n");
		usage();
	}
	if (!no_mfg_log) {
		if (!mac_address) {
			fprintf(stderr, "Missing mac_address\n");
			usage();
		}
		/* accept a non-delimited hex string MAC address */
		if (hex_parse(mac_bytes, sizeof(mac_bytes),
		    mac_address, &delim) < 0) {
			fprintf(stderr, "mac_address is not %zu hex bytes\n",
			    sizeof(mac_bytes));
			usage();
		} else if (delim) {
			fprintf(stderr, "mac_address should be undelimited "
			    "hex bytes\n");
			usage();
		}
	}
	if (verbosity < '0' || verbosity > '2') {
		fprintf(stderr, "Invalid verbosity: %c (range: 0-2)\n",
		    verbosity);
	}
}

int main(int argc, char **argv)
{
	char used_dsn_path[PATH_MAX];
	char log_path[PATH_MAX];
	char config_file_path[PATH_MAX];
	size_t len;

	gen_opts(argc, argv);

	log_init(cmdname, LOG_OPT_NO_SYSLOG);
	switch (verbosity) {
	case '2':
		log_set_options(LOG_OPT_DEBUG | LOG_OPT_FUNC_NAMES);
		/* no break */
	case '1':
		log_set_options(LOG_OPT_CONSOLE_OUT);
		/* no break */
	default:
		/* silent */
		break;
	}

	/* parse DSN file */
	if (gen_parse_dsn_file() < 0) {
		log_err("failed to load DSN file");
		return 1;
	}

	/* parse OEM info file */
	if (gen_parse_oem_info() < 0) {
		log_err("failed to load OEM info file");
		return 1;
	}

	/* set file paths */
	snprintf(log_path, sizeof(log_path), "%s/mfg_log.txt", output_dir);
	len = snprintf(used_dsn_path, sizeof(used_dsn_path), "%s/used_dsns",
	    output_dir);
	snprintf(config_file_path, sizeof(config_file_path), "%s/%s.conf",
	    output_dir, device_info.dsn);

	/* create output directory, if it doesn't exist */
	if (file_create_dir(output_dir, 0755) < 0) {
		log_err("failed to create output dir %s: %m", output_dir);
		return 1;
	}

	if (!no_mfg_log) {
		/* check for duplicate entries before proceeding */
		if (gen_validate_log_file(log_path, MFG_LOG_VERSION) < 0) {
			log_err("failed: log entry for device with DSN %s "
			    "already exists", device_info.dsn);
			return 1;
		}
	}

	/* create blank config file */
	conf_factory_edit_mode_enable();
	if (conf_save_empty(config_file_path) < 0 ||
	    conf_init(config_file_path, NULL) < 0 ||
	    conf_load() < 0) {
		log_err("failed to create config file %s", config_file_path);
		unlink(config_file_path);
		return 1;
	}

	/* write necessary values to config */
	if (gen_populate_config() < 0) {
		log_err("failed to populate config JSON");
		unlink(config_file_path);
		return 1;
	}

	/* save the config file to disk */
	if (conf_save() < 0) {
		log_err("failed to save config file %s", config_file_path);
		unlink(config_file_path);
		return 1;
	}

	if (!no_mfg_log) {
		/* append log entry to manufacturing log */
		if (gen_append_log_entry(log_path, MFG_LOG_VERSION) < 0) {
			log_err("failed to write log entry to %s", log_path);
			unlink(config_file_path);
			return 1;
		}
	}

	/* create used DSN directory, if it doesn't exist */
	if (file_create_dir(used_dsn_path, 0755) < 0) {
		log_err("failed to create used DSN dir %s: %m", used_dsn_path);
		return 1;
	}

	/* move DSN file to used_dsn directory */
	snprintf(used_dsn_path + len, sizeof(used_dsn_path) - len, "/%s.xml",
	    device_info.dsn);
	if (rename(dsn_path, used_dsn_path) < 0) {
		log_err("failed to move DSN file to %s: %m", used_dsn_path);
		return 1;
	}
	log_info("config generation for %s successful", device_info.dsn);
	return 0;
}

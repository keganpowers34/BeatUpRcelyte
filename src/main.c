#include "config.h"
#include "net.h"
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

static const char *const config_path = "config.json";

int main(int argc, char const *argv[]) {
	if(argc >= 2) {
		if(strcmp(argv[1], "-e") == 0 || strcmp(argv[1], "--example") == 0) {
			FILE *f = fopen(config_path, "w");
			if(f) {
				fprintf(f, "{\r\n\t\"master\": {\r\n\t\t\"domain\": \"example.com\",\r\n\t\t\"renew\": false,\r\n\t\t\"cert\": \"cert.pem\",\r\n\t\t\"key\": \"key.pem\"\r\n\t},\r\n\t\"status\": {\r\n\t\t\"domain\": \"status.example.com\",\r\n\t\t\"renew\": false,\r\n\t\t\"cert\": \"cert.pem\",\r\n\t\t\"key\": \"key.pem\"\r\n\t}\r\n}\r\n");
				fclose(f);
				fprintf(stderr, "Writing example %s\n", config_path);
				return 0;
			} else {
				fprintf(stderr, "Failed to open %s: %s\n", config_path, strerror(errno));
				return 0;
			}
		}
	}
	if(!isatty(0)) {
		fprintf(stderr, "Not running in an interactive terminal\n");
		return -1;
	}
	struct Config cfg = config_default();
	if(access(config_path, F_OK) == 0) {
		if(config_load(&cfg, config_path)) {
			fprintf(stderr, "Failed to load config\n");
			return -1;
		}
	}

	// TODO: use domain specific self-signed cert instead of generic one
	if(status_init())
		return -1;
	fprintf(stderr, "HTTP status started\n");
	if(status_ssl_init(&cfg.status_cert, &cfg.status_key, cfg.status_port))
		return -1;
	fprintf(stderr, "HTTPS status started\n");
	if(master_init(&cfg.master_cert, cfg.master_port))
		return -1;
	fprintf(stderr, "Master server started\nPress [enter] to exit\n");
	getchar();
	fprintf(stderr, "Stopping master server\n");
	master_cleanup();
	fprintf(stderr, "Stopping HTTPS status\n");
	status_ssl_cleanup();
	fprintf(stderr, "Stopping HTTP status\n");
	status_cleanup();
	config_free(&cfg);
	return 0;
}

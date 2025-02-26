#include "status/status.h"
#include "instance/instance.h"
#include "config.h"
#include "net.h"
#include "pool.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

static const char *config_path = "./config.json";
static bool headless = 0;

int main(int argc, const char *argv[]) {
	// fprintf(stderr, "MAX CODE: %u\n", StringToServerCode("99999", 5));
	for(const char **arg = &argv[1]; arg < &argv[argc]; ++arg) {
		if(strcmp(*arg, "--daemon") == 0) {
			headless = 1;
		} else if(strcmp(*arg, "-4") == 0 || strcmp(*arg, "--ipv4") == 0) {
			net_useIPv4 = 1;
		} else if(strcmp(*arg, "-c") == 0 || strcmp(*arg, "--config") == 0) {
			if(++arg < &argv[argc])
				config_path = *arg;
		}
	}
	if(headless == 0 && isatty(0) == 0) {
		fprintf(stderr, "Not running in an interactive terminal\n");
		return -1;
	}
	#ifdef WINDOWS
	if(headless) {
		fprintf(stderr, "Headless mode is only supported on POSIX systems\n");
		return -1;
	}
	#endif
	struct Config cfg;
	if(config_load(&cfg, config_path))
		goto fail0;
	status_internal_init();
	if(cfg.status_tls) {
		if(status_ssl_init(cfg.certs, cfg.keys, cfg.status_domain, cfg.status_path, cfg.status_port))
			goto fail1;
	} else {
		if(status_init(cfg.status_path, cfg.status_port))
			goto fail1;
	}
	if(instance_init(cfg.host_domain, cfg.host_domainIPv4))
		goto fail2;
	pool_init();
	if(master_init(cfg.certs, cfg.keys, cfg.master_port))
		goto fail3;
	if(headless) {
		#ifndef WINDOWS
		sigset_t sigset;
		sigemptyset(&sigset);
		sigaddset(&sigset, SIGINT);
		sigaddset(&sigset, SIGHUP);
		sigprocmask(SIG_BLOCK, &sigset, NULL);
		int sig;
		sigwait(&sigset, &sig);
		#endif
	} else {
		usleep(10000); // Fixes out of order logs
		fprintf(stderr, "Press [enter] to exit\n");
		getchar();
	}
	fail3:
	master_cleanup();
	fail2:
	instance_cleanup();
	fail1:
	if(cfg.status_tls)
		status_ssl_cleanup();
	else
		status_cleanup();
	fail0:
	config_free(&cfg);
	return 0;
}

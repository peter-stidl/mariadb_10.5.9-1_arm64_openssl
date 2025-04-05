#
# Regular cron jobs for the mariadb-10.5.9-arm64 package
#
0 4	* * *	root	[ -x /usr/bin/mariadb-10.5.9-arm64_maintenance ] && /usr/bin/mariadb-10.5.9-arm64_maintenance

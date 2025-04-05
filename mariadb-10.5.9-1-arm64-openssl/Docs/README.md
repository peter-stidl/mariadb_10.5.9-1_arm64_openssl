# MariaDB 10.5.9-1-arm64-openssl for ARM64 with OpenSSL (Compiled on Raspberry Pi 4)

This package provides MariaDB 10.5.9, a powerful open-source relational database management system (RDBMS), compiled for the ARM64 architecture with OpenSSL support. Built and tested on a Raspberry Pi 4, it includes performance optimizations, security enhancements, and full compatibility with MySQL. It is ideal for embedded systems, servers, and development environments that require a reliable SQL database solution.

## History

### Used Packages

- **Original Source Package:** MariaDB 10.5.9 ?? March 2021
  Includes several important security updates, such as a fix for the vulnerability **CVE-2021-27928**.
  _Source: lists.gnu.org_

- During the creation of MariaDB 10.5.9, certain components were removed, such as third-party libraries under `storage/tokudb/PerconaFT/third_party`. Corresponding CMake configurations were adjusted accordingly.
  _Source: lists.gnu.org_

- There were reports of installation issues for MariaDB 10.5.9, especially concerning repository sources. Some users recommended installing version 10.5.10 and then manually switching the repository configuration back to 10.5.9.
  _Source: Stack Overflow_

- For detailed build information and a list of available MariaDB 10.5 builds, refer to the MacPorts website.
  _Source: ports.macports.org_

### Original Source Files

- `mariadb-10.5.9.tar.gz` -- February 4, 2021
  `md5sum` - `d5ff75792dde53ba487567bd7e658198`
  `sha256sum` -- `40ab19aeb8de141fdc188cf2251213c9e7351bee4d0cd29db704fae68d1068cf`

- `openssl_1.1.1n-0+deb10u3_arm64.deb` -- March 15, 2022
  `md5sum` -- `a99eaff5cfd039745544077156b372e5`
  `sha256sum` -- `23d7f4ba9265a18e9e138f82fe6d23bffdab2be4eb1674f0d6bfbe318b145e1e`

- `pcre2-10.36.zip` -- December 5, 2020
  `md5sum` -- `ba9e743af42aac5642f7504b12af4116`
  `sha256sum` -- `3fb6d0981b6a87664810dbe625968151e42555e2983cf1c35b53a5245a5d22fc`

## Problem

During compilation, the link to the server hosting `pcre2-10.36.zip` was broken, as the file is no longer available at that location. The CMake script attempted to download it but failed repeatedly:

Scanning dependencies of target pcre2 [ 13%] Creating directories for 'pcre2' [ 13%] Performing download step (download, verify and extract) for 'pcre2' -- Using src='http://ftp.pcre.org/pub/pcre/pcre2-10.36.zip' -- [download 100% complete] -- Retrying...

Accessing the URL in a browser results in:

> http://ftp.pcre.org/pub/pcre/pcre2-10.36.zip
> 404 - Not found | We searched the space, but we could not find the page you are looking for.

## My Solution

To bypass the issue with the broken PCRE2 source URL, I placed the correct version of `pcre2-10.36.zip` (with verified `md5sum`) directly into the build directory. This ensured the build system could continue without external dependencies or failed downloads.

### Directory Structure

mariadb-10.5.9-1-arm64-openssl/     # Root directory of the MariaDB source code
|-- cmake/                          # CMake scripts and modules for configuration
|-- client/                         # MariaDB client files and tools 
|-- configs/                        # Example configuration files
|-- contrib/                        # Contributions and additional tools
|-- dbug/                           # Debugging-related files and functions
|-- debian/ 
    | -- extra/                     # External libraries and dependencies 
         | -- pcre2-10.36.zip
|-- doc/                            # Documentation
|-- extra/                          # External libraries and dependencies
    | -- pcre2/
         |-- pcre2-10.36/           # PCRE2 library sources
             | -- pcre2-10.36.zip   # PCRE2 ZIP file
|-- libmysql/                       # MariaDB client libraries
|-- mariadb/                        # Main source for MariaDB server
|-- scripts/                        # Build and install scripts
|-- storage/                        # Storage systems (e.g., InnoDB, MyISAM)
|-- sql/                            # SQL engines and functions
|-- tools/                          # Utilities and scripts (e.g., mysql_upgrade)
|-- CMakeLists.txt                  # Main CMake configuration file

## New Source Package

- `mariadb-10.5.9-1-arm64-openssl.orig.tar.gz`
  `md5sum` -- `1796a7b3ae9e0e0140bad0e81976d79d`
  `sha256sum` -- `16550091bce18344f0b74f00b67ce7204720a3af7134b55232fdffc0d269d261`


# Copyright (c) 2025 Metratec GmbH.
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/modem)
zephyr_library_include_directories(${ZEPHYR_BASE}/subsys/net/ip)
zephyr_library_include_directories(${ZEPHYR_BASE}/subsys/net/lib/sockets)
zephyr_library_sources(
    sim7600.c
    sim7600_pdp.c
    sim7600_dns.c
    sim7600_sock.c
    sim7600_ftp.c
    sim7600_gps.c
    sim7600_meas.c)

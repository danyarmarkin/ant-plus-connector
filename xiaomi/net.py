"""Route Xiaomi cloud requests through a specific network interface (e.g. en0), bypassing a VPN.

ru.api.io.mi.com is unreachable through the VPN tunnel but answers directly via Wi-Fi.
Two things are needed for that:
  * sockets bound to the interface (macOS IP_BOUND_IF), so only these sockets bypass the VPN;
  * DNS resolved via the interface's own DNS server: the system resolver returns VPN fake-IPs
    (198.18.0.0/15) that only work inside the tunnel.
TLS SNI and certificate checks still use the real host name.
"""
import os
import socket
import subprocess

import dns.resolver
import requests
import urllib3.util.connection as urllib3_conn
from requests.adapters import HTTPAdapter
from urllib3.connection import HTTPConnection

IP_BOUND_IF = 25  # macOS <netinet/in.h>
BIND_IFACE = os.environ.get("XIAOMI_IFACE", "en0")
DIRECT_DOMAINS = (".mi.com", ".xiaomi.com")

_resolver = None
_dns_cache = {}
_orig_create_connection = urllib3_conn.create_connection


def _iface_cmd(*args):
    try:
        return subprocess.check_output(["ipconfig", *args], text=True).strip()
    except subprocess.CalledProcessError:
        return ""


def _resolve_direct(host):
    if host not in _dns_cache:
        answers = _resolver.resolve(host, "A", source=_resolver.source_ip)
        _dns_cache[host] = [a.address for a in answers]
    return _dns_cache[host]


def _create_connection(address, *args, **kwargs):
    host, port = address
    if _resolver and host.endswith(DIRECT_DOMAINS):
        last_err = None
        for ip in _resolve_direct(host):
            try:
                return _orig_create_connection((ip, port), *args, **kwargs)
            except OSError as e:
                last_err = e
        raise last_err
    return _orig_create_connection(address, *args, **kwargs)


class BoundAdapter(HTTPAdapter):
    def __init__(self, iface, **kwargs):
        self._opts = HTTPConnection.default_socket_options + [
            (socket.IPPROTO_IP, IP_BOUND_IF, socket.if_nametoindex(iface))
        ]
        super().__init__(**kwargs)

    def send(self, request, **kwargs):
        if kwargs.get("timeout") is None:
            kwargs["timeout"] = 15  # some regions (us, sg) silently drop packets
        return super().send(request, **kwargs)

    def init_poolmanager(self, *args, **kwargs):
        kwargs["socket_options"] = self._opts
        super().init_poolmanager(*args, **kwargs)


def bind_session(session: requests.Session, iface: str = BIND_IFACE) -> bool:
    """Send all HTTPS requests of the session through iface; no-op if the interface is missing."""
    global _resolver
    if not iface:
        return False
    try:
        adapter = BoundAdapter(iface)
    except OSError:
        return False
    dns_server = _iface_cmd("getoption", iface, "domain_name_server")
    src_ip = _iface_cmd("getifaddr", iface)
    if dns_server and src_ip and _resolver is None:
        _resolver = dns.resolver.Resolver(configure=False)
        _resolver.nameservers = [dns_server]
        _resolver.timeout = _resolver.lifetime = 5
        _resolver.source_ip = src_ip
        urllib3_conn.create_connection = _create_connection
    session.mount("https://", adapter)
    return True

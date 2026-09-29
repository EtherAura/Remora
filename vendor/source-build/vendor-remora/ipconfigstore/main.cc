// ipconfigstore — write the framework's static-IP record for eth0 (bd remora-28ix.4 R2b).
//
// WHY THIS EXISTS AT ALL. A container's eth0 is configured by docker before Android boots, so the
// address is already correct at the interface — but the framework does not look there. Ethernet
// settings come from a file, /data/misc/ethernet/ipconfig.txt, which EthernetTracker parses at
// boot; with no file the framework treats eth0 as unconfigured, and DNS and proxy in particular
// then come from nowhere. This program reads the live interface, adds Remora's DNS/proxy boot
// args, and writes that file before netd starts (see remora.common.rc's post-fs-data stanza).
//
// THE FORMAT IS A CONTRACT WITH ANDROID'S OWN PARSER (frameworks IpConfigStore): a big-endian
// version integer, then key/value pairs where every string is a big-endian uint16 length followed
// by unterminated bytes, ending with the key "eos". It is not self-describing — a wrong length or
// a missing key does not produce an error, it produces a silently ignored file and an eth0 with
// no DNS. Version 3 is current; version 2 differs only in writing the id as an integer, and is
// kept for SDK <= 27 images.
//
// Reads Remora's OWN boot namespace (ro.boot.remora_net_*) directly — this is Remora's code now,
// so the adapter no longer has to translate for it.

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <set>
#include <string>

#include <android-base/properties.h>

using android::base::GetIntProperty;
using android::base::GetProperty;

namespace {

constexpr const char *kIface = "eth0";
constexpr const char *kOutPath = "/data/misc/ethernet/ipconfig.txt";

// ---- the packed encoding IpConfigStore expects -------------------------------------------
// Java's DataOutputStream is big-endian, so these are written big-endian regardless of host
// order. htons/htonl say that in one place instead of hand-rolling a byte-order test.

bool writeU16(FILE *f, uint16_t v) {
    const uint16_t be = htons(v);
    return fwrite(&be, sizeof(be), 1, f) == 1;
}

bool writeU32(FILE *f, uint32_t v) {
    const uint32_t be = htonl(v);
    return fwrite(&be, sizeof(be), 1, f) == 1;
}

// UTF-length-prefixed and NOT null-terminated — writeUTF's format. An empty string is a valid
// value (an unset proxy exclusion list is exactly that), so a zero length still writes.
bool writeString(FILE *f, const std::string &s) {
    if (!writeU16(f, static_cast<uint16_t>(s.size()))) return false;
    return s.empty() || fwrite(s.data(), s.size(), 1, f) == 1;
}

// ---- what the interface currently is ------------------------------------------------------

struct IpConfig {
    std::string address;      // dotted quad
    uint32_t prefixLength{0}; // bits set in the netmask
    std::string gateway;      // dotted quad, empty when there is no default route
};

int prefixLengthOf(const struct sockaddr *netmask) {
    if (netmask == nullptr) return 0;
    const uint32_t mask = reinterpret_cast<const struct sockaddr_in *>(netmask)->sin_addr.s_addr;
    return __builtin_popcount(mask);
}

// The default route's gateway for `iface`, from /proc/net/route. Fields are hex and
// LITTLE-endian-in-text there, which is why the address is copied raw into in_addr rather than
// parsed as a number and byte-swapped.
std::string defaultGateway(const char *iface) {
    FILE *f = fopen("/proc/net/route", "r");
    if (f == nullptr) return {};

    char line[512];
    if (fgets(line, sizeof(line), f) == nullptr) {  // header
        fclose(f);
        return {};
    }

    std::string result;
    while (fgets(line, sizeof(line), f) != nullptr) {
        char name[IFNAMSIZ + 1] = {};
        unsigned long dest = 0, gateway = 0;
        if (sscanf(line, "%16s %lx %lx", name, &dest, &gateway) != 3) continue;
        if (dest != 0 || strcmp(name, iface) != 0) continue;  // not the default route for us

        struct in_addr addr;
        addr.s_addr = static_cast<in_addr_t>(gateway);
        char buf[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &addr, buf, sizeof(buf)) != nullptr) result = buf;
        break;
    }

    fclose(f);
    return result;
}

bool readInterface(IpConfig *out) {
    struct ifaddrs *addrs = nullptr;
    if (getifaddrs(&addrs) != 0) {
        fprintf(stderr, "ipconfigstore: getifaddrs: %s\n", strerror(errno));
        return false;
    }

    bool found = false;
    for (struct ifaddrs *it = addrs; it != nullptr; it = it->ifa_next) {
        if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(it->ifa_name, kIface) != 0) continue;

        char buf[INET_ADDRSTRLEN] = {};
        const void *sin = &reinterpret_cast<struct sockaddr_in *>(it->ifa_addr)->sin_addr;
        if (inet_ntop(AF_INET, sin, buf, sizeof(buf)) == nullptr) continue;
        out->address = buf;
        out->prefixLength = static_cast<uint32_t>(prefixLengthOf(it->ifa_netmask));
        found = true;
        break;
    }

    freeifaddrs(addrs);
    if (!found) fprintf(stderr, "ipconfigstore: no IPv4 address on %s\n", kIface);
    out->gateway = defaultGateway(kIface);
    return found;
}

// ---- the record ---------------------------------------------------------------------------

void writeDns(FILE *f) {
    // A set, so a repeated resolver is written once — the framework treats duplicates as distinct
    // servers and would query the same address twice. Ordering is the set's, which is stable.
    std::set<std::string> servers;
    const int count = GetIntProperty("ro.boot.remora_net_ndns", 0);
    for (int i = 1; i <= count; ++i) {
        const std::string dns = GetProperty("ro.boot.remora_net_dns" + std::to_string(i), "");
        if (!dns.empty()) servers.insert(dns);
    }
    // A static configuration with NO resolver cannot resolve anything, and the framework will not
    // fall back on its own, so an unset list gets a working public default rather than silence.
    if (servers.empty()) servers.insert("8.8.8.8");

    for (const std::string &dns : servers) {
        writeString(f, "dns");
        writeString(f, dns);
    }
}

void writeProxy(FILE *f) {
    const std::string type = GetProperty("ro.boot.remora_net_proxy_type", "");
    if (type == "static") {
        writeString(f, "proxySettings");
        writeString(f, "STATIC");
        writeString(f, "proxyHost");
        writeString(f, GetProperty("ro.boot.remora_net_proxy_host", ""));
        writeString(f, "proxyPort");
        writeU32(f, static_cast<uint32_t>(GetIntProperty("ro.boot.remora_net_proxy_port", 3128)));
        writeString(f, "exclusionList");
        writeString(f, GetProperty("ro.boot.remora_net_proxy_exclude_list", ""));
    } else if (type == "pac") {
        writeString(f, "proxySettings");
        writeString(f, "PAC");
        writeString(f, "proxyPac");
        writeString(f, GetProperty("ro.boot.remora_net_proxy_pac", ""));
    } else if (type == "none") {
        writeString(f, "proxySettings");
        writeString(f, "NONE");
    }
    // Anything else — including unset — writes no proxy keys at all, which is not the same as
    // NONE: it leaves the framework's own default in place instead of asserting "no proxy".
}

bool writeRecord(const IpConfig &conf, uint32_t version) {
    FILE *f = fopen(kOutPath, "w");
    if (f == nullptr) {
        fprintf(stderr, "ipconfigstore: cannot write %s: %s\n", kOutPath, strerror(errno));
        return false;
    }

    writeU32(f, version);

    writeString(f, "ipAssignment");
    writeString(f, "STATIC");

    writeString(f, "linkAddress");
    writeString(f, conf.address);
    writeU32(f, conf.prefixLength);

    // The route record is positional: destination prefix, then whether a gateway follows. The
    // 0.0.0.0/0 destination is what makes this the DEFAULT route.
    writeString(f, "gateway");
    writeU32(f, 1);  // a destination follows
    writeString(f, "0.0.0.0");
    writeU32(f, 0);  // prefix length 0 — default route
    writeU32(f, conf.gateway.empty() ? 0 : 1);
    if (!conf.gateway.empty()) writeString(f, conf.gateway);

    writeDns(f);
    writeProxy(f);

    writeString(f, "id");
    if (version == 2)
        writeU32(f, 0);
    else
        writeString(f, kIface);

    writeString(f, "eos");

    const bool ok = ferror(f) == 0;
    if (fclose(f) != 0 || !ok) {
        fprintf(stderr, "ipconfigstore: short write to %s\n", kOutPath);
        return false;
    }
    return true;
}

}  // namespace

int main(int, char **) {
    // Version 2 wrote the id as an integer; 3 writes the interface name. Only ancient images
    // need 2, and Remora builds none of them — kept because the cost is one comparison and the
    // failure it prevents (a file the parser silently rejects) is invisible.
    const uint32_t version = GetIntProperty("ro.build.version.sdk", 0) <= 27 ? 2 : 3;

    IpConfig conf;
    if (!readInterface(&conf)) {
        // No address yet is not a reason to write a broken record: a file naming an empty
        // address is worse than no file, because the framework accepts it and then has nothing.
        return 1;
    }

    printf("ipconfigstore: %s/%u via %s (v%u)\n", conf.address.c_str(), conf.prefixLength,
           conf.gateway.empty() ? "(no gateway)" : conf.gateway.c_str(), version);
    return writeRecord(conf, version) ? 0 : 1;
}

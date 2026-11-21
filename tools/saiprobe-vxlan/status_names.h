/* generated from saistatus.h by gen_status_names.sh */
#include <stdio.h>
static const char *saiprobe_status_name(int s, char *buf, size_t n)
{
    switch (s) {
    case 0x00000000: return "SAI_STATUS_SUCCESS";
    case -0x00000001: return "SAI_STATUS_FAILURE";
    case -0x00000002: return "SAI_STATUS_NOT_SUPPORTED";
    case -0x00000003: return "SAI_STATUS_NO_MEMORY";
    case -0x00000004: return "SAI_STATUS_INSUFFICIENT_RESOURCES";
    case -0x00000005: return "SAI_STATUS_INVALID_PARAMETER";
    case -0x00000006: return "SAI_STATUS_ITEM_ALREADY_EXISTS";
    case -0x00000007: return "SAI_STATUS_ITEM_NOT_FOUND";
    case -0x00000008: return "SAI_STATUS_BUFFER_OVERFLOW";
    case -0x00000009: return "SAI_STATUS_INVALID_PORT_NUMBER";
    case -0x0000000A: return "SAI_STATUS_INVALID_PORT_MEMBER";
    case -0x0000000B: return "SAI_STATUS_INVALID_VLAN_ID";
    case -0x0000000C: return "SAI_STATUS_UNINITIALIZED";
    case -0x0000000D: return "SAI_STATUS_TABLE_FULL";
    case -0x0000000E: return "SAI_STATUS_MANDATORY_ATTRIBUTE_MISSING";
    case -0x0000000F: return "SAI_STATUS_NOT_IMPLEMENTED";
    case -0x00000010: return "SAI_STATUS_ADDR_NOT_FOUND";
    case -0x00000011: return "SAI_STATUS_OBJECT_IN_USE";
    case -0x00000012: return "SAI_STATUS_INVALID_OBJECT_TYPE";
    case -0x00000013: return "SAI_STATUS_INVALID_OBJECT_ID";
    case -0x00000014: return "SAI_STATUS_INVALID_NV_STORAGE";
    case -0x00000015: return "SAI_STATUS_NV_STORAGE_FULL";
    case -0x00000016: return "SAI_STATUS_SW_UPGRADE_VERSION_MISMATCH";
    case -0x00000017: return "SAI_STATUS_NOT_EXECUTED";
    case -0x00010000: return "SAI_STATUS_INVALID_ATTRIBUTE_0";
    case -0x00020000: return "SAI_STATUS_INVALID_ATTR_VALUE_0";
    case -0x00030000: return "SAI_STATUS_ATTR_NOT_IMPLEMENTED_0";
    case -0x00040000: return "SAI_STATUS_UNKNOWN_ATTRIBUTE_0";
    case -0x00050000: return "SAI_STATUS_ATTR_NOT_SUPPORTED_0";
    default: break;
    }
    {
        unsigned u = (unsigned)(-s);
        static const char *rng[] = { 0, "INVALID_ATTRIBUTE", "INVALID_ATTR_VALUE", "ATTR_NOT_IMPLEMENTED", "UNKNOWN_ATTRIBUTE", "ATTR_NOT_SUPPORTED" };
        if (s < 0 && (u >> 16) >= 1 && (u >> 16) <= 5) { snprintf(buf, n, "SAI_STATUS_%s_%u", rng[u >> 16], u & 0xffff); return buf; }
    }
    snprintf(buf, n, "SAI_STATUS_UNKNOWN(%d)", s);
    return buf;
}

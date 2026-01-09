#ifndef DNSSD_H
#define DNSSD_H

#if defined(WIN32) && defined(DLL_EXPORT)
# define DNSSD_API __declspec(dllexport)
#else
# define DNSSD_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define DNSSD_ERROR_NOERROR       0
#define DNSSD_ERROR_HWADDRLEN     1
#define DNSSD_ERROR_OUTOFMEM      2
#define DNSSD_ERROR_LIBNOTFOUND   3
#define DNSSD_ERROR_PROCNOTFOUND  4

typedef struct dnssd_s dnssd_t;

DNSSD_API dnssd_t *dnssd_init(const char *name, int name_len, const char *hw_addr, int hw_addr_len, int *error);

DNSSD_API int dnssd_register_raop(dnssd_t *dnssd, unsigned short port);
DNSSD_API int dnssd_register_airplay(dnssd_t *dnssd, unsigned short port);

DNSSD_API void dnssd_unregister_raop(dnssd_t *dnssd);
DNSSD_API void dnssd_unregister_airplay(dnssd_t *dnssd);

DNSSD_API const char *dnssd_get_airplay_txt(dnssd_t *dnssd, int *length);
DNSSD_API const char *dnssd_get_name(dnssd_t *dnssd, int *length);
DNSSD_API const char *dnssd_get_hw_addr(dnssd_t *dnssd, int *length);

/* Browsing functions for service discovery */
/* Forward declarations - actual types are in dns_sd.h when available */
struct sockaddr;
typedef void (*dnssd_browse_reply_t)(void *sdRef, uint32_t flags, uint32_t interfaceIndex,
                                     int32_t errorCode, const char *serviceName,
                                     const char *regtype, const char *replyDomain, void *context);
typedef void (*dnssd_resolve_reply_t)(void *sdRef, uint32_t flags, uint32_t interfaceIndex,
                                       int32_t errorCode, const char *fullname,
                                       const char *hosttarget, uint16_t port,
                                       uint16_t txtLen, const unsigned char *txtRecord, void *context);
typedef void (*dnssd_addrinfo_reply_t)(void *sdRef, uint32_t flags, uint32_t interfaceIndex,
                                        int32_t errorCode, const char *hostname,
                                        const struct sockaddr *address, uint32_t ttl, void *context);

DNSSD_API void *dnssd_browse_start(dnssd_t *dnssd, const char *regtype, dnssd_browse_reply_t callback, void *context);
DNSSD_API void dnssd_browse_stop(dnssd_t *dnssd, void *browseRef);
DNSSD_API void *dnssd_resolve_start(dnssd_t *dnssd, const char *name, const char *regtype, const char *domain, dnssd_resolve_reply_t callback, void *context);
DNSSD_API void dnssd_resolve_stop(dnssd_t *dnssd, void *resolveRef);
DNSSD_API void *dnssd_getaddrinfo_start(dnssd_t *dnssd, const char *hostname, dnssd_addrinfo_reply_t callback, void *context);
DNSSD_API void dnssd_getaddrinfo_stop(dnssd_t *dnssd, void *addrinfoRef);
DNSSD_API int dnssd_txt_get_value(dnssd_t *dnssd, const unsigned char *txtRecord, uint16_t txtLen, const char *key, uint8_t *valueLen, const void **value);

DNSSD_API int dnssd_process_result(dnssd_t *dnssd, void *serviceRef);

DNSSD_API void dnssd_destroy(dnssd_t *dnssd);

#ifdef __cplusplus
}
#endif
#endif
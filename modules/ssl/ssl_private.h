/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SSL_PRIVATE_H
#define SSL_PRIVATE_H

/**
 * @file  ssl_private.h
 * @brief Internal interfaces private to mod_ssl.
 *
 * @defgroup MOD_SSL_PRIVATE Private
 * @ingroup MOD_SSL
 * @{
 */

/** Apache headers */
#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_main.h"
#include "http_connection.h"
#include "http_request.h"
#include "http_protocol.h"
#include "http_vhost.h"
#include "util_script.h"
#include "util_filter.h"
#include "util_ebcdic.h"
#include "util_mutex.h"
#include "apr.h"
#include "apr_strings.h"
#define APR_WANT_STRFUNC
#define APR_WANT_MEMFUNC
#include "apr_want.h"
#include "apr_tables.h"
#include "apr_lib.h"
#include "apr_fnmatch.h"
#include "apr_strings.h"
#include "apr_global_mutex.h"
#include "apr_optional.h"
#include "ap_socache.h"
#include "mod_auth.h"

/* The #ifdef macros are only defined AFTER including the above
 * therefore we cannot include these system files at the top  :-(
 */
#if APR_HAVE_STDLIB_H
#include <stdlib.h>
#endif
#if APR_HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#if APR_HAVE_UNISTD_H
#include <unistd.h> /* needed for STDIN_FILENO et.al., at least on FreeBSD */
#endif

#ifndef FALSE
#define FALSE 0
#endif

#ifndef TRUE
#define TRUE !FALSE
#endif

#ifndef BOOL
#define BOOL unsigned int
#endif

#include "ap_expr.h"

/* OpenSSL headers */
#include <openssl/opensslv.h>
#if (OPENSSL_VERSION_NUMBER >= 0x10001000)
/* must be defined before including ssl.h */
#define OPENSSL_NO_SSL_INTERN
#endif
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>
#include <openssl/x509_vfy.h>
#include <openssl/ocsp.h>

/* Avoid tripping over an engine build installed globally and detected
 * when the user points at an explicit non-engine flavor of OpenSSL
 */
#if defined(HAVE_OPENSSL_ENGINE_H) && defined(HAVE_ENGINE_INIT)
#include <openssl/engine.h>
#endif

#if (OPENSSL_VERSION_NUMBER < 0x0090801f)
#error mod_ssl requires OpenSSL 0.9.8a or later
#endif

/**
 * ...shifting sands of OpenSSL...
 * Note: when adding support for new OpenSSL features, avoid explicit
 * version number checks whenever possible, and use "feature-based"
 * detection instead (check for definitions of constants or functions)
 */
#if (OPENSSL_VERSION_NUMBER >= 0x10000000)
#define MODSSL_SSL_CIPHER_CONST const
#define MODSSL_SSL_METHOD_CONST const
#else
#define MODSSL_SSL_CIPHER_CONST
#define MODSSL_SSL_METHOD_CONST
#endif

#if defined(OPENSSL_FIPS)
#define HAVE_FIPS
#endif

#if defined(SSL_OP_NO_TLSv1_2)
#define HAVE_TLSV1_X
#endif

#if defined(SSL_CONF_FLAG_FILE)
#define HAVE_SSL_CONF_CMD
#endif

/**
  * The following features all depend on TLS extension support.
  * Within this block, check again for features (not version numbers).
  */
#if !defined(OPENSSL_NO_TLSEXT) && defined(SSL_set_tlsext_host_name)

#define HAVE_TLSEXT

/* ECC: make sure we have at least 1.0.0 */
#if !defined(OPENSSL_NO_EC) && defined(TLSEXT_ECPOINTFORMAT_uncompressed)
#define HAVE_ECC
#endif

/* OCSP stapling */
#if !defined(OPENSSL_NO_OCSP) && defined(SSL_CTX_set_tlsext_status_cb)
#define HAVE_OCSP_STAPLING
/* All exist but are no longer macros since OpenSSL 1.1.0 */
#if OPENSSL_VERSION_NUMBER < 0x10100000L
/* backward compatibility with OpenSSL < 1.0 */
#ifndef sk_OPENSSL_STRING_num
#define sk_OPENSSL_STRING_num sk_num
#endif
#ifndef sk_OPENSSL_STRING_value
#define sk_OPENSSL_STRING_value sk_value
#endif
#ifndef sk_OPENSSL_STRING_pop
#define sk_OPENSSL_STRING_pop sk_pop
#endif
#endif /* if OPENSSL_VERSION_NUMBER < 0x10100000L */
#endif /* if !defined(OPENSSL_NO_OCSP) && defined(SSL_CTX_set_tlsext_status_cb) */

/* session id constness */
#if OPENSSL_VERSION_NUMBER < 0x10100000L
#define IDCONST
#else
#define IDCONST const
#endif

/* TLS session tickets */
#if defined(SSL_CTX_set_tlsext_ticket_key_cb)
#define HAVE_TLS_SESSION_TICKETS
#define TLSEXT_TICKET_KEY_LEN 48
#ifndef tlsext_tick_md
#ifdef OPENSSL_NO_SHA256
#define tlsext_tick_md EVP_sha1
#else
#define tlsext_tick_md EVP_sha256
#endif
#endif
#endif

/* Secure Remote Password */
#if !defined(OPENSSL_NO_SRP) && defined(SSL_CTRL_SET_TLS_EXT_SRP_USERNAME_CB)
#define HAVE_SRP
#include <openssl/srp.h>
#endif

/* ALPN Protocol Negotiation */
#if defined(TLSEXT_TYPE_application_layer_protocol_negotiation)
#define HAVE_TLS_ALPN
#endif

#endif /* !defined(OPENSSL_NO_TLSEXT) && defined(SSL_set_tlsext_host_name) */

#if OPENSSL_VERSION_NUMBER < 0x10100000L
#define BIO_set_init(x,v)     (x->init=v)
#define BIO_get_data(x)       (x->ptr)
#define BIO_set_data(x,v)     (x->ptr=v)
#define BIO_get_shutdown(x)   (x->shutdown)
#define BIO_set_shutdown(x,v) (x->shutdown=v)
#define DH_bits(x)            (BN_num_bits(x->p))
#else
void init_bio_methods(void);
void free_bio_methods(void);
#endif

/* mod_ssl headers */
#include "ssl_util_ssl.h"

APLOG_USE_MODULE(ssl);

/*
 * Provide reasonable default for some defines
 */
#ifndef PFALSE
#define PFALSE ((void *)FALSE)
#endif
#ifndef PTRUE
#define PTRUE ((void *)TRUE)
#endif
#ifndef UNSET
#define UNSET (-1)
#endif
#ifndef NUL
#define NUL '\0'
#endif
#ifndef RAND_MAX
#include <limits.h>
#define RAND_MAX INT_MAX
#endif

/**
 * Provide reasonable defines for some types
 */
#ifndef UCHAR
#define UCHAR unsigned char
#endif

/**
 * Provide useful shorthands
 */
#define strEQ(s1,s2)     (strcmp(s1,s2)        == 0)
#define strNE(s1,s2)     (strcmp(s1,s2)        != 0)
#define strEQn(s1,s2,n)  (strncmp(s1,s2,n)     == 0)
#define strNEn(s1,s2,n)  (strncmp(s1,s2,n)     != 0)

#define strcEQ(s1,s2)    (strcasecmp(s1,s2)    == 0)
#define strcNE(s1,s2)    (strcasecmp(s1,s2)    != 0)
#define strcEQn(s1,s2,n) (strncasecmp(s1,s2,n) == 0)
#define strcNEn(s1,s2,n) (strncasecmp(s1,s2,n) != 0)

#define strIsEmpty(s)    (s == NULL || s[0] == NUL)

#define myConnConfig(c) \
(SSLConnRec *)ap_get_module_config(c->conn_config, &ssl_module)
#define myCtxConfig(sslconn, sc) (sslconn->is_proxy ? sc->proxy : sc->server)
#define myConnConfigSet(c, val) \
ap_set_module_config(c->conn_config, &ssl_module, val)
#define mySrvConfig(srv) (SSLSrvConfigRec *)ap_get_module_config(srv->module_config,  &ssl_module)
#define myDirConfig(req) (SSLDirConfigRec *)ap_get_module_config(req->per_dir_config, &ssl_module)
#define myModConfig(srv) (mySrvConfig((srv)))->mc
#define mySrvFromConn(c) (myConnConfig(c))->server
#define mySrvConfigFromConn(c) mySrvConfig(mySrvFromConn(c))
#define myModConfigFromConn(c) myModConfig(mySrvFromConn(c))

/**
 * Defaults for the configuration
 */
#ifndef SSL_SESSION_CACHE_TIMEOUT
#define SSL_SESSION_CACHE_TIMEOUT  300
#endif

/* Default setting for per-dir reneg buffer. */
#ifndef DEFAULT_RENEG_BUFFER_SIZE
#define DEFAULT_RENEG_BUFFER_SIZE (128 * 1024)
#endif

/* Default for OCSP response validity */
#ifndef DEFAULT_OCSP_MAX_SKEW
#define DEFAULT_OCSP_MAX_SKEW (60 * 5)
#endif

/* Default timeout for OCSP queries */
#ifndef DEFAULT_OCSP_TIMEOUT
#define DEFAULT_OCSP_TIMEOUT 10
#endif

/**
 * Define the SSL options
 */
#define SSL_OPT_NONE           (0)
#define SSL_OPT_RELSET         (1<<0)
#define SSL_OPT_STDENVVARS     (1<<1)
#define SSL_OPT_EXPORTCERTDATA (1<<3)
#define SSL_OPT_FAKEBASICAUTH  (1<<4)
#define SSL_OPT_STRICTREQUIRE  (1<<5)
#define SSL_OPT_OPTRENEGOTIATE (1<<6)
#define SSL_OPT_LEGACYDNFORMAT (1<<7)
typedef int ssl_opt_t;

/**
 * Define the SSL Protocol options
 */
#define SSL_PROTOCOL_NONE  (0)
#ifndef OPENSSL_NO_SSL3
#define SSL_PROTOCOL_SSLV3 (1<<1)
#endif
#define SSL_PROTOCOL_TLSV1 (1<<2)
#ifndef OPENSSL_NO_SSL3
#define SSL_PROTOCOL_BASIC (SSL_PROTOCOL_SSLV3|SSL_PROTOCOL_TLSV1)
#else
#define SSL_PROTOCOL_BASIC (SSL_PROTOCOL_TLSV1)
#endif
#ifdef HAVE_TLSV1_X
#define SSL_PROTOCOL_TLSV1_1 (1<<3)
#define SSL_PROTOCOL_TLSV1_2 (1<<4)
#define SSL_PROTOCOL_ALL   (SSL_PROTOCOL_BASIC| \
                            SSL_PROTOCOL_TLSV1_1|SSL_PROTOCOL_TLSV1_2)
#else
#define SSL_PROTOCOL_ALL   (SSL_PROTOCOL_BASIC)
#endif
#ifndef OPENSSL_NO_SSL3
#define SSL_PROTOCOL_DEFAULT (SSL_PROTOCOL_ALL & ~SSL_PROTOCOL_SSLV3)
#else
#define SSL_PROTOCOL_DEFAULT (SSL_PROTOCOL_ALL)
#endif
typedef int ssl_proto_t;

/**
 * Define the SSL verify levels
 */
typedef enum {
    SSL_CVERIFY_UNSET           = UNSET,
    SSL_CVERIFY_NONE            = 0,
    SSL_CVERIFY_OPTIONAL        = 1,
    SSL_CVERIFY_REQUIRE         = 2,
    SSL_CVERIFY_OPTIONAL_NO_CA  = 3
} ssl_verify_t;

#define SSL_VERIFY_PEER_STRICT \
     (SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT)

#define ssl_verify_error_is_optional(errnum) \
   ((errnum == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT) \
    || (errnum == X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN) \
    || (errnum == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY) \
    || (errnum == X509_V_ERR_CERT_UNTRUSTED) \
    || (errnum == X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE))

/**
  * CRL checking mask (mode | flags)
  */
typedef enum {
    SSL_CRLCHECK_NONE  = (0),
    SSL_CRLCHECK_LEAF  = (1 << 0),
    SSL_CRLCHECK_CHAIN = (1 << 1),

#define SSL_CRLCHECK_FLAGS (~0x3)
    SSL_CRLCHECK_NO_CRL_FOR_CERT_OK = (1 << 2)
} ssl_crlcheck_t;

/**
 * Define the SSL pass phrase dialog types
 */
typedef enum {
    SSL_PPTYPE_UNSET   = UNSET,
    SSL_PPTYPE_BUILTIN = 0,
    SSL_PPTYPE_FILTER  = 1,
    SSL_PPTYPE_PIPE    = 2
} ssl_pphrase_t;

/**
 * Define the Path Checking modes
 */
#define SSL_PCM_EXISTS     1
#define SSL_PCM_ISREG      2
#define SSL_PCM_ISDIR      4
#define SSL_PCM_ISNONZERO  8
typedef unsigned int ssl_pathcheck_t;

/**
 * Define the SSL enabled state
 */
typedef enum {
    SSL_ENABLED_UNSET    = UNSET,
    SSL_ENABLED_FALSE    = 0,
    SSL_ENABLED_TRUE     = 1,
    SSL_ENABLED_OPTIONAL = 3
} ssl_enabled_t;

/**
 * Define the SSL requirement structure
 */
typedef struct {
    const char     *cpExpr;
    ap_expr_info_t *mpExpr;
} ssl_require_t;

/**
 * Define the SSL random number generator seeding source
 */
typedef enum {
    SSL_RSCTX_STARTUP = 1,
    SSL_RSCTX_CONNECT = 2
} ssl_rsctx_t;
typedef enum {
    SSL_RSSRC_BUILTIN = 1,
    SSL_RSSRC_FILE    = 2,
    SSL_RSSRC_EXEC    = 3,
    SSL_RSSRC_EGD     = 4
} ssl_rssrc_t;
typedef struct {
    ssl_rsctx_t  nCtx;
    ssl_rssrc_t  nSrc;
    char        *cpPath;
    int          nBytes;
} ssl_randseed_t;

/**
 * Define the structure of an ASN.1 anything
 */
typedef struct {
    long int       nData;
    unsigned char *cpData;
    apr_time_t     source_mtime;
} ssl_asn1_t;

/**
 * Define the mod_ssl per-module configuration structure
 * (i.e. the global configuration for each httpd process)
 */

typedef enum {
    SSL_SHUTDOWN_TYPE_UNSET,
    SSL_SHUTDOWN_TYPE_STANDARD,
    SSL_SHUTDOWN_TYPE_UNCLEAN,
    SSL_SHUTDOWN_TYPE_ACCURATE
} ssl_shutdown_type_e;

typedef struct {
    SSL *ssl;
    const char *client_dn;
    X509 *client_cert;
    ssl_shutdown_type_e shutdown_type;
    const char *verify_info;
    const char *verify_error;
    int verify_depth;
    int is_proxy;
    int disabled;
    enum {
        NON_SSL_OK = 0,        /* is SSL request, or error handling completed */
        NON_SSL_SEND_REQLINE,  /* Need to send the fake request line */
        NON_SSL_SEND_HDR_SEP,  /* Need to send the header separator */
        NON_SSL_SET_ERROR_MSG  /* Need to set the error message */
    } non_ssl_request;

    /* Track the handshake/renegotiation state for the connection so
     * that all client-initiated renegotiations can be rejected, as a
     * partial fix for CVE-2009-3555. */
    enum {
        RENEG_INIT = 0, /* Before initial handshake */
        RENEG_REJECT,   /* After initial handshake; any client-initiated
                         * renegotiation should be rejected */
        RENEG_ALLOW,    /* A server-initiated renegotiation is taking
                         * place (as dictated by configuration) */
        RENEG_ABORT     /* Renegotiation initiated by client, abort the
                         * connection */
    } reneg_state;

    server_rec *server;
    
    const char *cipher_suite; /* cipher suite used in last reneg */
} SSLConnRec;

/* BIG FAT WARNING: SSLModConfigRec has unusual memory lifetime: it is
 * allocated out of the "process" pool and only a single such
 * structure is created and used for the lifetime of the process.
 * (The process pool is s->process->pool and is stored in the .pPool
 * field.)  Most members of this structure are likewise allocated out
 * of the process pool, but notably sesscache and sesscache_context
 * are not.
 *
 * The structure is treated as mostly immutable after a single config
 * parse has completed; the post_config hook (ssl_init_Module) flips
 * the bFixed flag to true and subsequent invocations of the config
 * callbacks hence do nothing.
 *
 * This odd lifetime strategy is used so that encrypted private keys
 * can be decrypted once at startup and continue to be used across
 * subsequent server reloads where the interactive password prompt is
 * not possible.

 * It is really an ABI nightmare waiting to happen since DSOs are
 * reloaded across restarts, and nothing prevents the struct type
 * changing across such reloads, yet the cached structure will be
 * assumed to match regardless.
 *
 * This should really be fixed using a smaller structure which only
 * stores that which is absolutely necessary (the private keys, maybe
 * the random seed), and have that structure be strictly ABI-versioned
 * for safety.
 */
typedef struct {
    pid_t           pid;
    apr_pool_t     *pPool;
    BOOL            bFixed;

    /* OpenSSL SSL_SESS_CACHE_* flags: */
    long            sesscache_mode;

    /* The configured provider, and associated private data
     * structure. */
    const ap_socache_provider_t *sesscache;
    ap_socache_instance_t *sesscache_context;

    apr_global_mutex_t   *pMutex;
    apr_array_header_t   *aRandSeed;
    apr_hash_t     *tVHostKeys;

    /* A hash table of pointers to ssl_asn1_t structures.  The structures
     * are used to store private keys in raw DER format (serialized OpenSSL
     * PrivateKey structures).  The table is indexed by (vhost-id,
     * index), for example the string "vhost.example.com:443:0". */
    apr_hash_t     *tPrivateKey;

#if defined(HAVE_OPENSSL_ENGINE_H) && defined(HAVE_ENGINE_INIT)
    const char     *szCryptoDevice;
#endif

#ifdef HAVE_OCSP_STAPLING
    const ap_socache_provider_t *stapling_cache;
    ap_socache_instance_t *stapling_cache_context;
    apr_global_mutex_t   *stapling_cache_mutex;
    apr_global_mutex_t   *stapling_refresh_mutex;
#endif
} SSLModConfigRec;

/** Structure representing configured filenames for certs and keys for
 * a given vhost */
typedef struct {
    /* Lists of configured certs and keys for this server */
    apr_array_header_t *cert_files;
    apr_array_header_t *key_files;

    /** Certificates which specify the set of CA names which should be
     * sent in the CertificateRequest message: */
    const char  *ca_name_path;
    const char  *ca_name_file;
} modssl_pk_server_t;

typedef struct {
    /** proxy can have any number of cert/key pairs */
    const char  *cert_file;
    const char  *cert_path;
    const char  *ca_cert_file;
    STACK_OF(X509_INFO) *certs; /* Contains End Entity certs */
    STACK_OF(X509) **ca_certs; /* Contains ONLY chain certs for
                                * each item in certs.
                                * (ptr to array of ptrs) */
} modssl_pk_proxy_t;

/** stuff related to authentication that can also be per-dir */
typedef struct {
    /** known/trusted CAs */
    const char  *ca_cert_path;
    const char  *ca_cert_file;

    const char  *cipher_suite;

    /** for client or downstream server authentication */
    int          verify_depth;
    ssl_verify_t verify_mode;
} modssl_auth_ctx_t;

#ifdef HAVE_TLS_SESSION_TICKETS
typedef struct {
    const char *file_path;
    unsigned char key_name[16];
    unsigned char hmac_secret[16];
    unsigned char aes_key[16];
} modssl_ticket_key_t;
#endif

#ifdef HAVE_SSL_CONF_CMD
typedef struct {
    const char *name;
    const char *value;
} ssl_ctx_param_t;
#endif

typedef struct SSLSrvConfigRec SSLSrvConfigRec;

typedef struct {
    SSLSrvConfigRec *sc; /** pointer back to server config */
    SSL_CTX *ssl_ctx;

    /** we are one or the other */
    modssl_pk_server_t *pks;
    modssl_pk_proxy_t  *pkp;

#ifdef HAVE_TLS_SESSION_TICKETS
    modssl_ticket_key_t *ticket_key;
#endif

    ssl_proto_t  protocol;
    int protocol_set;

    /** config for handling encrypted keys */
    ssl_pphrase_t pphrase_dialog_type;
    const char   *pphrase_dialog_path;

    const char  *cert_chain;

    /** certificate revocation list */
    const char    *crl_path;
    const char    *crl_file;
    int            crl_check_mask;

#ifdef HAVE_OCSP_STAPLING
    /** OCSP stapling options */
    BOOL        stapling_enabled;
    long        stapling_resptime_skew;
    long        stapling_resp_maxage;
    int         stapling_cache_timeout;
    BOOL        stapling_return_errors;
    BOOL        stapling_fake_trylater;
    int         stapling_errcache_timeout;
    apr_interval_time_t stapling_responder_timeout;
    const char *stapling_force_url;
#endif

#ifdef HAVE_SRP
    char *srp_vfile;
    char *srp_unknown_user_seed;
    SRP_VBASE  *srp_vbase;
#endif

    modssl_auth_ctx_t auth;

    BOOL ocsp_enabled; /* true if OCSP verification enabled */
    BOOL ocsp_force_default; /* true if the default responder URL is
                              * used regardless of per-cert URL */
    const char *ocsp_responder; /* default responder URL */
    long ocsp_resptime_skew;
    long ocsp_resp_maxage;
    apr_interval_time_t ocsp_responder_timeout;
    BOOL ocsp_use_request_nonce;
    apr_uri_t *proxy_uri;

#ifdef HAVE_SSL_CONF_CMD
    SSL_CONF_CTX *ssl_ctx_config; /* Configuration context */
    apr_array_header_t *ssl_ctx_param; /* parameters to pass to SSL_CTX */
#endif
} modssl_ctx_t;

struct SSLSrvConfigRec {
    SSLModConfigRec *mc;
    ssl_enabled_t    enabled;
    BOOL             proxy_enabled;
    const char      *vhost_id;
    int              vhost_id_len;
    int              session_cache_timeout;
    BOOL             cipher_server_pref;
    BOOL             insecure_reneg;
    modssl_ctx_t    *server;
    modssl_ctx_t    *proxy;
    ssl_enabled_t    proxy_ssl_check_peer_expire;
    ssl_enabled_t    proxy_ssl_check_peer_cn;
    ssl_enabled_t    proxy_ssl_check_peer_name;
#ifdef HAVE_TLSEXT
    ssl_enabled_t    strict_sni_vhost_check;
#endif
#ifdef HAVE_FIPS
    BOOL             fips;
#endif
#ifndef OPENSSL_NO_COMP
    BOOL             compression;
#endif
    BOOL             session_tickets;
};

/**
 * Define the mod_ssl per-directory configuration structure
 * (i.e. the local configuration for all &lt;Directory>
 *  and .htaccess contexts)
 */
typedef struct {
    BOOL          bSSLRequired;
    apr_array_header_t *aRequirement;
    ssl_opt_t     nOptions;
    ssl_opt_t     nOptionsAdd;
    ssl_opt_t     nOptionsDel;
    const char   *szCipherSuite;
    ssl_verify_t  nVerifyClient;
    int           nVerifyDepth;
    const char   *szCACertificatePath;
    const char   *szCACertificateFile;
    const char   *szUserName;
    apr_size_t    nRenegBufferSize;
} SSLDirConfigRec;

/**
 *  function prototypes
 */

/**  API glue structures  */
extern module AP_MODULE_DECLARE_DATA ssl_module;

/**  configuration handling   */
SSLModConfigRec *ssl_config_global_create(server_rec *);
void         ssl_config_global_fix(SSLModConfigRec *);
BOOL         ssl_config_global_isfixed(SSLModConfigRec *);
void        *ssl_config_server_create(apr_pool_t *, server_rec *);
void        *ssl_config_server_merge(apr_pool_t *, void *, void *);
void        *ssl_config_perdir_create(apr_pool_t *, char *);
void        *ssl_config_perdir_merge(apr_pool_t *, void *, void *);
const char  *ssl_cmd_SSLPassPhraseDialog(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLCryptoDevice(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLRandomSeed(cmd_parms *, void *, const char *, const char *, const char *);
const char  *ssl_cmd_SSLEngine(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLCipherSuite(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLCertificateFile(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLCertificateKeyFile(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLCertificateChainFile(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLCACertificatePath(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLCACertificateFile(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLCADNRequestPath(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLCADNRequestFile(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLCARevocationPath(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLCARevocationFile(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLCARevocationCheck(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLHonorCipherOrder(cmd_parms *cmd, void *dcfg, int flag);
const char  *ssl_cmd_SSLCompression(cmd_parms *, void *, int flag);
const char  *ssl_cmd_SSLSessionTickets(cmd_parms *, void *, int flag);
const char  *ssl_cmd_SSLVerifyClient(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLVerifyDepth(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLSessionCache(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLSessionCacheTimeout(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLProtocol(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLOptions(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLRequireSSL(cmd_parms *, void *);
const char  *ssl_cmd_SSLRequire(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLUserName(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLRenegBufferSize(cmd_parms *cmd, void *dcfg, const char *arg);
const char  *ssl_cmd_SSLStrictSNIVHostCheck(cmd_parms *cmd, void *dcfg, int flag);
const char *ssl_cmd_SSLInsecureRenegotiation(cmd_parms *cmd, void *dcfg, int flag);

const char  *ssl_cmd_SSLProxyEngine(cmd_parms *cmd, void *dcfg, int flag);
const char  *ssl_cmd_SSLProxyProtocol(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLProxyCipherSuite(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLProxyVerify(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLProxyVerifyDepth(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLProxyCACertificatePath(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLProxyCACertificateFile(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLProxyCARevocationPath(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLProxyCARevocationFile(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLProxyCARevocationCheck(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLProxyMachineCertificatePath(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLProxyMachineCertificateFile(cmd_parms *, void *, const char *);
const char  *ssl_cmd_SSLProxyMachineCertificateChainFile(cmd_parms *, void *, const char *);
#ifdef HAVE_TLS_SESSION_TICKETS
const char *ssl_cmd_SSLSessionTicketKeyFile(cmd_parms *cmd, void *dcfg, const char *arg);
#endif
const char  *ssl_cmd_SSLProxyCheckPeerExpire(cmd_parms *cmd, void *dcfg, int flag);
const char  *ssl_cmd_SSLProxyCheckPeerCN(cmd_parms *cmd, void *dcfg, int flag);
const char  *ssl_cmd_SSLProxyCheckPeerName(cmd_parms *cmd, void *dcfg, int flag);

const char *ssl_cmd_SSLOCSPOverrideResponder(cmd_parms *cmd, void *dcfg, int flag);
const char *ssl_cmd_SSLOCSPDefaultResponder(cmd_parms *cmd, void *dcfg, const char *arg);
const char *ssl_cmd_SSLOCSPResponseTimeSkew(cmd_parms *cmd, void *dcfg, const char *arg);
const char *ssl_cmd_SSLOCSPResponseMaxAge(cmd_parms *cmd, void *dcfg, const char *arg);
const char *ssl_cmd_SSLOCSPResponderTimeout(cmd_parms *cmd, void *dcfg, const char *arg);
const char *ssl_cmd_SSLOCSPUseRequestNonce(cmd_parms *cmd, void *dcfg, int flag);
const char *ssl_cmd_SSLOCSPEnable(cmd_parms *cmd, void *dcfg, int flag);
const char *ssl_cmd_SSLOCSPProxyURL(cmd_parms *cmd, void *dcfg, const char *arg);

#ifdef HAVE_SSL_CONF_CMD
const char *ssl_cmd_SSLOpenSSLConfCmd(cmd_parms *cmd, void *dcfg, const char *arg1, const char *arg2);
#endif

#ifdef HAVE_SRP
const char *ssl_cmd_SSLSRPVerifierFile(cmd_parms *cmd, void *dcfg, const char *arg);
const char *ssl_cmd_SSLSRPUnknownUserSeed(cmd_parms *cmd, void *dcfg, const char *arg);
#endif

const char *ssl_cmd_SSLFIPS(cmd_parms *cmd, void *dcfg, int flag);

/**  module initialization  */
apr_status_t ssl_init_Module(apr_pool_t *, apr_pool_t *, apr_pool_t *, server_rec *);
apr_status_t ssl_init_Engine(server_rec *, apr_pool_t *);
apr_status_t ssl_init_ConfigureServer(server_rec *, apr_pool_t *, apr_pool_t *, SSLSrvConfigRec *,
                                      apr_array_header_t *);
apr_status_t ssl_init_CheckServers(server_rec *, apr_pool_t *);
STACK_OF(X509_NAME)
            *ssl_init_FindCAList(server_rec *, apr_pool_t *, const char *, const char *);
void         ssl_init_Child(apr_pool_t *, server_rec *);
apr_status_t ssl_init_ModuleKill(void *data);

/**  Apache API hooks  */
int          ssl_hook_Auth(request_rec *);
int          ssl_hook_UserCheck(request_rec *);
int          ssl_hook_Access(request_rec *);
int          ssl_hook_Fixup(request_rec *);
int          ssl_hook_ReadReq(request_rec *);
int          ssl_hook_Upgrade(request_rec *);
void         ssl_hook_ConfigTest(apr_pool_t *pconf, server_rec *s);

/** Apache authz provisders */
extern const authz_provider ssl_authz_provider_require_ssl;
extern const authz_provider ssl_authz_provider_verify_client;

/**  OpenSSL callbacks */
DH          *ssl_callback_TmpDH(SSL *, int, int);
int          ssl_callback_SSLVerify(int, X509_STORE_CTX *);
int          ssl_callback_SSLVerify_CRL(int, X509_STORE_CTX *, conn_rec *);
int          ssl_callback_proxy_cert(SSL *ssl, X509 **x509, EVP_PKEY **pkey);
int          ssl_callback_NewSessionCacheEntry(SSL *, SSL_SESSION *);
SSL_SESSION *ssl_callback_GetSessionCacheEntry(SSL *, IDCONST unsigned char *, int, int *);
void         ssl_callback_DelSessionCacheEntry(SSL_CTX *, SSL_SESSION *);
void         ssl_callback_Info(const SSL *, int, int);
#ifdef HAVE_TLSEXT
int          ssl_callback_ServerNameIndication(SSL *, int *, modssl_ctx_t *);
#endif
#ifdef HAVE_TLS_SESSION_TICKETS
int         ssl_callback_SessionTicket(SSL *, unsigned char *, unsigned char *,
                                       EVP_CIPHER_CTX *, HMAC_CTX *, int);
#endif

#ifdef HAVE_TLS_ALPN
int ssl_callback_alpn_select(SSL *ssl, const unsigned char **out,
                             unsigned char *outlen, const unsigned char *in,
                             unsigned int inlen, void *arg);
#endif

/**  Session Cache Support  */
apr_status_t ssl_scache_init(server_rec *, apr_pool_t *);
void         ssl_scache_status_register(apr_pool_t *p);
void         ssl_scache_kill(server_rec *);
BOOL         ssl_scache_store(server_rec *, IDCONST UCHAR *, int,
                              apr_time_t, SSL_SESSION *, apr_pool_t *);
SSL_SESSION *ssl_scache_retrieve(server_rec *, IDCONST UCHAR *, int, apr_pool_t *);
void         ssl_scache_remove(server_rec *, IDCONST UCHAR *, int,
                               apr_pool_t *);

/** OCSP Stapling Support */
#ifdef HAVE_OCSP_STAPLING
const char *ssl_cmd_SSLStaplingCache(cmd_parms *, void *, const char *);
const char *ssl_cmd_SSLUseStapling(cmd_parms *, void *, int);
const char *ssl_cmd_SSLStaplingResponseTimeSkew(cmd_parms *, void *, const char *);
const char *ssl_cmd_SSLStaplingResponseMaxAge(cmd_parms *, void *, const char *);
const char *ssl_cmd_SSLStaplingStandardCacheTimeout(cmd_parms *, void *, const char *);
const char *ssl_cmd_SSLStaplingErrorCacheTimeout(cmd_parms *, void *, const char *);
const char *ssl_cmd_SSLStaplingReturnResponderErrors(cmd_parms *, void *, int);
const char *ssl_cmd_SSLStaplingFakeTryLater(cmd_parms *, void *, int);
const char *ssl_cmd_SSLStaplingResponderTimeout(cmd_parms *, void *, const char *);
const char *ssl_cmd_SSLStaplingForceURL(cmd_parms *, void *, const char *);
apr_status_t modssl_init_stapling(server_rec *, apr_pool_t *, apr_pool_t *, modssl_ctx_t *);
void         ssl_stapling_certinfo_hash_init(apr_pool_t *);
int          ssl_stapling_init_cert(server_rec *, apr_pool_t *, apr_pool_t *,
                                    modssl_ctx_t *, X509 *);
#endif
#ifdef HAVE_SRP
int          ssl_callback_SRPServerParams(SSL *, int *, void *);
#endif

/**  I/O  */
void         ssl_io_filter_init(conn_rec *, request_rec *r, SSL *);
void         ssl_io_filter_register(apr_pool_t *);
long         ssl_io_data_cb(BIO *, int, const char *, int, long, long);

/* ssl_io_buffer_fill fills the setaside buffering of the HTTP request
 * to allow an SSL renegotiation to take place. */
int          ssl_io_buffer_fill(request_rec *r, apr_size_t maxlen);

/**  PRNG  */
int          ssl_rand_seed(server_rec *, apr_pool_t *, ssl_rsctx_t, char *);

/**  Utility Functions  */
char        *ssl_util_vhostid(apr_pool_t *, server_rec *);
apr_file_t  *ssl_util_ppopen(server_rec *, apr_pool_t *, const char *,
                             const char * const *);
void         ssl_util_ppclose(server_rec *, apr_pool_t *, apr_file_t *);
char        *ssl_util_readfilter(server_rec *, apr_pool_t *, const char *,
                                 const char * const *);
BOOL         ssl_util_path_check(ssl_pathcheck_t, const char *, apr_pool_t *);
#if OPENSSL_VERSION_NUMBER < 0x10100000L
void         ssl_util_thread_setup(apr_pool_t *);
#endif
int          ssl_init_ssl_connection(conn_rec *c, request_rec *r);

BOOL         ssl_util_vhost_matches(const char *servername, server_rec *s);

/**  Pass Phrase Support  */
apr_status_t ssl_load_encrypted_pkey(server_rec *, apr_pool_t *, int,
                                     const char *, apr_array_header_t **);

/**  Diffie-Hellman Parameter Support  */
DH           *ssl_dh_GetParamFromFile(const char *);
#ifdef HAVE_ECC
EC_GROUP     *ssl_ec_GetParamFromFile(const char *);
#endif

unsigned char *ssl_asn1_table_set(apr_hash_t *table,
                                  const char *key,
                                  long int length);

ssl_asn1_t *ssl_asn1_table_get(apr_hash_t *table,
                               const char *key);

void ssl_asn1_table_unset(apr_hash_t *table,
                          const char *key);

/**  Mutex Support  */
int          ssl_mutex_init(server_rec *, apr_pool_t *);
int          ssl_mutex_reinit(server_rec *, apr_pool_t *);
int          ssl_mutex_on(server_rec *);
int          ssl_mutex_off(server_rec *);

int          ssl_stapling_mutex_reinit(server_rec *, apr_pool_t *);

/* mutex type names for Mutex directive */
#define SSL_CACHE_MUTEX_TYPE    "ssl-cache"
#define SSL_STAPLING_CACHE_MUTEX_TYPE "ssl-stapling"
#define SSL_STAPLING_REFRESH_MUTEX_TYPE "ssl-stapling-refresh"

apr_status_t ssl_die(server_rec *);

/**  Logfile Support  */
void         ssl_log_ssl_error(const char *, int, int, server_rec *);

/* ssl_log_xerror, ssl_log_cxerror and ssl_log_rxerror are wrappers for the
 * respective ap_log_*error functions and take a certificate as an
 * additional argument (whose details are appended to the log message).
 * The other arguments are interpreted exactly as with their ap_log_*error
 * counterparts. */
void ssl_log_xerror(const char *file, int line, int level,
                    apr_status_t rv, apr_pool_t *p, server_rec *s,
                    X509 *cert, const char *format, ...)
    __attribute__((format(printf,8,9)));

void ssl_log_cxerror(const char *file, int line, int level,
                     apr_status_t rv, conn_rec *c, X509 *cert,
                     const char *format, ...)
    __attribute__((format(printf,7,8)));

void ssl_log_rxerror(const char *file, int line, int level,
                     apr_status_t rv, request_rec *r, X509 *cert,
                     const char *format, ...)
    __attribute__((format(printf,7,8)));

#define SSLLOG_MARK              __FILE__,__LINE__

/**  Variables  */

/* Register variables for the lifetime of the process pool 'p'. */
void         ssl_var_register(apr_pool_t *p);
char        *ssl_var_lookup(apr_pool_t *, server_rec *, conn_rec *, request_rec *, char *);
apr_array_header_t *ssl_ext_list(apr_pool_t *p, conn_rec *c, int peer, const char *extension);

void         ssl_var_log_config_register(apr_pool_t *p);

/* Extract SSL_*_DN_* variables into table 't' from SSL object 'ssl',
 * allocating from 'p': */
void modssl_var_extract_dns(apr_table_t *t, SSL *ssl, apr_pool_t *p);

/* Extract SSL_*_SAN_* variables (subjectAltName entries) into table 't'
 * from SSL object 'ssl', allocating from 'p'. */
void modssl_var_extract_san_entries(apr_table_t *t, SSL *ssl, apr_pool_t *p);

#ifndef OPENSSL_NO_OCSP
/* Perform OCSP validation of the current cert in the given context.
 * Returns non-zero on success or zero on failure.  On failure, the
 * context error code is set. */
int modssl_verify_ocsp(X509_STORE_CTX *ctx, SSLSrvConfigRec *sc,
                       server_rec *s, conn_rec *c, apr_pool_t *pool);

/* OCSP helper interface; dispatches the given OCSP request to the
 * responder at the given URI.  Returns the decoded OCSP response
 * object, or NULL on error (in which case, errors will have been
 * logged).  Pool 'p' is used for temporary allocations. */
OCSP_RESPONSE *modssl_dispatch_ocsp_request(const apr_uri_t *uri,
                                            apr_interval_time_t timeout,
                                            OCSP_REQUEST *request,
                                            conn_rec *c, apr_pool_t *p);
#endif

/* Retrieve DH parameters for given key length.  Return value should
 * be treated as unmutable, since it is stored in process-global
 * memory. */
DH *modssl_get_dh_params(unsigned keylen);

#if HAVE_VALGRIND
extern int ssl_running_on_valgrind;
#endif

#endif /* SSL_PRIVATE_H */
/** @} */


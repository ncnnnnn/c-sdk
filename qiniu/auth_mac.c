/*
 ============================================================================
 Name        : mac_auth.c
 Author      : Qiniu.com
 Copyright   : 2012(c) Shanghai Qiniu Information Technologies Co., Ltd.
 Description :
 ============================================================================
 */

#include "http.h"
#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/engine.h>

#if defined(_WIN32)
#pragma comment(lib, "libcrypto.lib")
#endif

/*============================================================================*/
/* Global */

void Qiniu_MacAuth_Init()
{
	ENGINE_load_builtin_engines();
	ENGINE_register_all_complete();
}

void Qiniu_MacAuth_Cleanup()
{
}

void Qiniu_Servend_Init(long flags)
{
	Qiniu_Global_Init(flags);
	Qiniu_MacAuth_Init();
}

void Qiniu_Servend_Cleanup()
{
	Qiniu_Global_Cleanup();
}

/*============================================================================*/
/* type Qiniu_Mac */

static void Qiniu_Mac_Hmac_inner(Qiniu_Mac *mac, const char *items[], size_t items_len, const char *addition, size_t addlen, char *digest, unsigned int *digest_len)
{
#if OPENSSL_VERSION_NUMBER < 0x10100000
	HMAC_CTX ctx;
	HMAC_CTX_init(&ctx);
	HMAC_Init_ex(&ctx, mac->secretKey, strlen(mac->secretKey), EVP_sha1(), NULL);
	for (size_t i = 0; i < items_len; i++)
	{
		HMAC_Update(&ctx, items[i], strlen(items[i]));
	}
	HMAC_Update(&ctx, "\n", 1);
	if (addlen > 0)
	{
		HMAC_Update(&ctx, addition, addlen);
	}
	HMAC_Final(&ctx, digest, digest_len);
	HMAC_cleanup(&ctx);

#endif

#if OPENSSL_VERSION_NUMBER > 0x10100000
	HMAC_CTX *ctx = HMAC_CTX_new();
	HMAC_Init_ex(ctx, mac->secretKey, strlen(mac->secretKey), EVP_sha1(), NULL);
	for (size_t i = 0; i < items_len; i++)
	{
		HMAC_Update(ctx, items[i], strlen(items[i]));
	}
	HMAC_Update(ctx, "\n", 1);
	if (addlen > 0)
	{
		HMAC_Update(ctx, addition, addlen);
	}
	HMAC_Final(ctx, digest, digest_len);
	HMAC_CTX_free(ctx);
#endif
}

static void Qiniu_Mac_Hmac(Qiniu_Mac *mac, const char *path, const char *addition, size_t addlen, char *digest, unsigned int *digest_len)
{
	const char *items[] = {path};
	Qiniu_Mac_Hmac_inner(mac, items, sizeof(items) / sizeof(const char *), addition, addlen, digest, digest_len);
}

static void Qiniu_Mac_HmacV2(Qiniu_Mac *mac, const char *method, const char *host, const char *path, const char *contentType, const char *addition, size_t addlen, char *digest, unsigned int *digest_len)
{
	const char *items[] = {method, " ", path, "\nHost: ", host, "\nContent-Type: ", contentType, "\n"};
	Qiniu_Mac_Hmac_inner(mac, items, sizeof(items) / sizeof(const char *), addition, addlen, digest, digest_len);
}

static Qiniu_Error Qiniu_Mac_Parse_Url(const char *url, char const **pHost, size_t *pHostLen, char const **pPath, size_t *pPathLen)
{
	Qiniu_Error err;
	char const *path = strstr(url, "://");
	char const *host = NULL;
	size_t hostLen = 0;

	if (path != NULL)
	{
		host = path + 3;
		path = strchr(path + 3, '/');
	}
	if (path == NULL)
	{
		err.code = 400;
		err.message = "invalid url";
		return err;
	}
	hostLen = path - host;

	if (pHost != NULL)
	{
		*pHost = host;
	}
	if (pHostLen != NULL)
	{
		*pHostLen = hostLen;
	}
	if (pPath != NULL)
	{
		*pPath = path;
	}
	if (pPathLen != NULL)
	{
		*pPathLen = strlen(path);
	}

	return Qiniu_OK;
}

static Qiniu_Error Qiniu_Mac_Auth(
	void *self, Qiniu_Header **header, const char *url, const char *addition, size_t addlen)
{
	Qiniu_Error err;
	char *auth;
	char *enc_digest;
	char digest[EVP_MAX_MD_SIZE + 1];
	unsigned int digest_len = sizeof(digest);
	Qiniu_Mac mac;

	char const *path;
	err = Qiniu_Mac_Parse_Url(url, NULL, NULL, &path, NULL);
	if (err.code != 200)
	{
		return err;
	}

	if (self)
	{
		mac = *(Qiniu_Mac *)self;
	}
	else
	{
		mac.accessKey = QINIU_ACCESS_KEY;
		mac.secretKey = QINIU_SECRET_KEY;
	}
	Qiniu_Mac_Hmac(&mac, path, addition, addlen, digest, &digest_len);
	enc_digest = Qiniu_Memory_Encode(digest, digest_len);

	auth = Qiniu_String_Concat("Authorization: QBox ", mac.accessKey, ":", enc_digest, NULL);
	Qiniu_Free(enc_digest);

	*header = curl_slist_append(*header, auth);
	Qiniu_Free(auth);

	return Qiniu_OK;
}

static const char *APPLICATION_OCTET_STREAM = "application/octet-stream";

static Qiniu_Error Qiniu_Mac_AuthV2(
	void *self, const char *method, Qiniu_Header **header, const char *contentType, const char *url, const char *addition, size_t addlen)
{
	Qiniu_Error err;
	char *auth;
	char *enc_digest;
	char digest[EVP_MAX_MD_SIZE + 1];
	unsigned int digest_len = sizeof(digest);
	Qiniu_Mac mac;

	char const *host = NULL;
	size_t hostLen = 0;
	char const *path = NULL;
	err = Qiniu_Mac_Parse_Url(url, &host, &hostLen, &path, NULL);
	if (err.code != 200)
	{
		return err;
	}

	char *normalizedHost = (char *)malloc(hostLen + 1);
	memcpy((void *)normalizedHost, host, hostLen);
	*(normalizedHost + hostLen) = '\0';

	if (self)
	{
		mac = *(Qiniu_Mac *)self;
	}
	else
	{
		mac.accessKey = QINIU_ACCESS_KEY;
		mac.secretKey = QINIU_SECRET_KEY;
	}

	if (strcmp(contentType, APPLICATION_OCTET_STREAM) == 0)
	{
		addlen = 0;
	}

	Qiniu_Mac_HmacV2(&mac, method, normalizedHost, path, contentType, addition, addlen, digest, &digest_len);
	Qiniu_Free(normalizedHost);
	enc_digest = Qiniu_Memory_Encode(digest, digest_len);

	auth = Qiniu_String_Concat("Authorization: Qiniu ", mac.accessKey, ":", enc_digest, NULL);
	Qiniu_Free(enc_digest);

	if (*header == NULL)
	{
		const char *contentTypeHeader = Qiniu_String_Concat("Content-Type: ", contentType, NULL);
		*header = curl_slist_append(*header, contentTypeHeader);
		Qiniu_Free((void *)contentTypeHeader);
	}
	*header = curl_slist_append(*header, auth);
	Qiniu_Free(auth);

	return Qiniu_OK;
}

static void Qiniu_Mac_Release(void *self)
{
	if (self)
	{
		free(self);
	}
}

static Qiniu_Mac *Qiniu_Mac_Clone(Qiniu_Mac *mac)
{
	Qiniu_Mac *p;
	char *accessKey;
	size_t n1, n2;
	if (mac)
	{
		n1 = strlen(mac->accessKey) + 1;
		n2 = strlen(mac->secretKey) + 1;
		p = (Qiniu_Mac *)malloc(sizeof(Qiniu_Mac) + n1 + n2);
		accessKey = (char *)(p + 1);
		memcpy(accessKey, mac->accessKey, n1);
		memcpy(accessKey + n1, mac->secretKey, n2);
		p->accessKey = accessKey;
		p->secretKey = accessKey + n1;
		return p;
	}
	return NULL;
}

static Qiniu_Auth_Itbl Qiniu_MacAuth_Itbl = {
	Qiniu_Mac_Auth,
	Qiniu_Mac_Release,
	Qiniu_Mac_AuthV2};

Qiniu_Auth Qiniu_MacAuth(Qiniu_Mac *mac)
{
	Qiniu_Auth auth = {Qiniu_Mac_Clone(mac), &Qiniu_MacAuth_Itbl};
	return auth;
};

void Qiniu_Client_InitMacAuth(Qiniu_Client *self, size_t bufSize, Qiniu_Mac *mac)
{
	Qiniu_Auth auth = {Qiniu_Mac_Clone(mac), &Qiniu_MacAuth_Itbl};
	Qiniu_Client_InitEx(self, auth, bufSize);
}

/*============================================================================*/
/* func Qiniu_Mac_Sign*/

char *Qiniu_Mac_Sign(Qiniu_Mac *self, char *data)
{
	char *sign;
	char *encoded_digest;
	char digest[EVP_MAX_MD_SIZE + 1];
	unsigned int digest_len = sizeof(digest);

	Qiniu_Mac mac;

	if (self)
	{
		mac = *self;
	}
	else
	{
		mac.accessKey = QINIU_ACCESS_KEY;
		mac.secretKey = QINIU_SECRET_KEY;
	}

#if OPENSSL_VERSION_NUMBER < 0x10100000
	HMAC_CTX ctx;
	HMAC_CTX_init(&ctx);
	HMAC_Init_ex(&ctx, mac.secretKey, strlen(mac.secretKey), EVP_sha1(), NULL);
	HMAC_Update(&ctx, data, strlen(data));
	HMAC_Final(&ctx, digest, &digest_len);
	HMAC_CTX_cleanup(&ctx);
#endif
#if OPENSSL_VERSION_NUMBER > 0x10100000
	HMAC_CTX *ctx = HMAC_CTX_new();
	HMAC_Init_ex(ctx, mac.secretKey, strlen(mac.secretKey), EVP_sha1(), NULL);
	HMAC_Update(ctx, data, strlen(data));
	HMAC_Final(ctx, digest, &digest_len);
	HMAC_CTX_free(ctx);
#endif
	encoded_digest = Qiniu_Memory_Encode(digest, digest_len);
	sign = Qiniu_String_Concat3(mac.accessKey, ":", encoded_digest);
	Qiniu_Free(encoded_digest);

	return sign;
}

/*============================================================================*/
/* func Qiniu_Mac_SignToken */

char *Qiniu_Mac_SignToken(Qiniu_Mac *self, char *policy_str)
{
	char *data;
	char *sign;
	char *token;

	data = Qiniu_String_Encode(policy_str);
	sign = Qiniu_Mac_Sign(self, data);
	token = Qiniu_String_Concat3(sign, ":", data);

	Qiniu_Free(sign);
	Qiniu_Free(data);

	return token;
}

/*============================================================================*/

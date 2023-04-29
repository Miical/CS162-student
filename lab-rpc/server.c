/**
 * Server binary.
 */

#include "kv_store.h"
#include <glib.h>
#include <memory.h>
#include <netinet/in.h>
#include <rpc/pmap_clnt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#ifndef SIG_PF
#define SIG_PF void (*)(int)
#endif

/* TODO: Add global state. */
GHashTable *ht;

extern void kvstore_1(struct svc_req *, struct SVCXPRT *);

/* Set up and run RPC server. */
int main(int argc, char **argv) {
  register SVCXPRT *transp;

  pmap_unset(KVSTORE, KVSTORE_V1);

  transp = svcudp_create(RPC_ANYSOCK);
  if (transp == NULL) {
    fprintf(stderr, "%s", "cannot create udp service.");
    exit(1);
  }
  if (!svc_register(transp, KVSTORE, KVSTORE_V1, kvstore_1, IPPROTO_UDP)) {
    fprintf(stderr, "%s", "unable to register (KVSTORE, KVSTORE_V1, udp).");
    exit(1);
  }

  transp = svctcp_create(RPC_ANYSOCK, 0, 0);
  if (transp == NULL) {
    fprintf(stderr, "%s", "cannot create tcp service.");
    exit(1);
  }
  if (!svc_register(transp, KVSTORE, KVSTORE_V1, kvstore_1, IPPROTO_TCP)) {
    fprintf(stderr, "%s", "unable to register (KVSTORE, KVSTORE_V1, tcp).");
    exit(1);
  }

  /* TODO: Initialize state. */
  ht = g_hash_table_new(g_bytes_hash, g_bytes_equal);

  svc_run();
  fprintf(stderr, "%s", "svc_run returned");
  exit(1);
  /* NOTREACHED */
}

/* Example server-side RPC stub. */
int *example_1_svc(int *argp, struct svc_req *rqstp) {
  static int result;

  result = *argp + 1;

  return &result;
}

/* TODO: Add additional RPC stubs. */

/* Echo server-side RPC stub. */
char ** echo_1_svc(char **argp, struct svc_req *rqstp) {
  static char* origin_string;

  origin_string = *argp;

  return &origin_string;
}

/* Put server-side RPC stub. */
void *put_1_svc(put_request* argp, struct svc_req *rqstp) {
  static void *result;

  GBytes *gkey = g_bytes_new(argp->key.buf_val, argp->key.buf_len);
  GBytes *gvalue = g_bytes_new(argp->value.buf_val, argp->value.buf_len);
  g_hash_table_insert(ht, gkey, gvalue);

  return &result;
}

/* Get server-side RPC stub. */
buf *get_1_svc(buf *key, struct svc_req *rqstp) {
  static buf result;

  if (result.buf_val != NULL) {
    free(result.buf_val);
    result.buf_val = NULL;
  }

  GBytes *gkey = g_bytes_new(key->buf_val, key->buf_len);
  GBytes *gvalue = g_hash_table_lookup(ht, gkey);

  g_bytes_unref(gkey);

  if (gvalue != NULL) {
    long unsigned int len;
    const char *data = g_bytes_get_data(gvalue, &len);

    result.buf_val = (char *) malloc(len * sizeof(char));
    result.buf_len = len;
    memcpy(result.buf_val, data, len);
  } else {
    result.buf_val = NULL;
    result.buf_len = 0;
  }

  return &result;
}

#ifndef PB_H
#define PB_H
/* the shared video player (vp_net.c) asks the host app to configure its curl handles */
void pb_curl_prepare(void *curl);
#endif

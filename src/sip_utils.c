/*
    Copyright (C) 2002  Thomas Ries <tries@gmx.net>

    This file is part of Siproxd.
    
    Siproxd is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
    
    Siproxd is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with Siproxd; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA 
*/
#include "config.h"

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <net/if.h>
#include <sys/ioctl.h>

#include <sys/types.h>
#include <pwd.h>

#include <osipparser2/osip_parser.h>
#include <osipparser2/osip_port.h>

#include "siproxd.h"
#include "rewrite_rules.h"
#include "log.h"

static char const ident[]="$Id: " __FILE__ ": " PACKAGE "-" VERSION "-"\
			  BUILDSTR " $";

/* configuration storage */
extern struct siproxd_config configuration;

extern int h_errno;
extern int sip_socket;				/* sending SIP datagrams */


/*
 * create a reply template from an given SIP request
 *
 * RETURNS a pointer to osip_message_t
 */
osip_message_t *msg_make_template_reply (osip_message_t * request, int code) {
   osip_message_t *response;
   int pos;

   osip_message_init (&response);
   response->message=NULL;
   osip_message_set_version (response, osip_strdup ("SIP/2.0"));
   osip_message_set_status_code (response, code);
   osip_message_set_reason_phrase (response, 
                                   osip_strdup(osip_message_get_reason (code)));

   if (request->to==NULL) {
      ERROR("msg_make_template_reply: empty To in request header");
   }

   if (request->from==NULL) {
      ERROR("msg_make_template_reply: empty From in request header");
   }

   osip_to_clone (request->to, &response->to);
   osip_from_clone (request->from, &response->from);


   /* via headers */
   pos = 0;
   while (!osip_list_eol (request->vias, pos)) {
      char *tmp;
      osip_via_t *via;
      via = (osip_via_t *) osip_list_get (request->vias, pos);
      osip_via_to_str (via, &tmp);

      osip_message_set_via (response, tmp);
      osip_free (tmp);
      pos++;
   }

   osip_call_id_clone(request->call_id,&response->call_id);
   osip_cseq_clone(request->cseq,&response->cseq);

   return response;
}


/*
 * check for a via loop.
 * It checks for the presense of a via entry that holds one of
 * my IP addresses and is *not* the topmost via.
 *
 * RETURNS
 *	STS_TRUE if loop detected
 *	STS_FALSE if no loop
 */
int check_vialoop (osip_message_t *my_msg) {
/*
!!! actually this is a problematic one.
1) for requests, I must search the whole VIA list
   (topmost via is the previos station in the path)

2) for responses I must skip the topmost via, as this is mine
   (and will be removed later on)

3) What happens if we have 'clashes'  with private addresses??
   From that point of view, siproxd *should* not try to
   check against it's local IF addresses if thei are private.
   this then of course again can lead to a endless loop...
   
   can we use something like a Tag in via headers?? (a veriy likely
   to-be-unique ID)

*/
   int sts;
   int pos;
   int found_own_via;

   found_own_via=0;
   pos = 1;	/* for detecting a loop, don't check the first entry 
   		   as this is my own VIA! */
   while (!osip_list_eol (my_msg->vias, pos)) {
      osip_via_t *via;
      via = (osip_via_t *) osip_list_get (my_msg->vias, pos);
      sts = is_via_local (via);
      if (sts == STS_TRUE) found_own_via=1;
      pos++;
   }
   return (found_own_via)? STS_TRUE : STS_FALSE;
}


/*
 * check if a given osip_via_t is local. I.e. its address is owned
 * by my inbound or outbound interface
 *
 * RETURNS
 *	STS_TRUE if the given VIA is one of my interfaces
 *	STS_FALSE otherwise
 */
int is_via_local (osip_via_t *via) {
   int sts, found;
   struct in_addr addr_via, addr_myself;
   char *my_interfaces[]=
        { configuration.inbound_if,  configuration.outbound_if,  (char*)-1 };
   int port;
   int i;
   char *ptr;

   if (via==NULL) {
      ERROR("called is_via_local with NULL via");
      return STS_FALSE;
   }

   DEBUGC(DBCLASS_BABBLE,"via name %s",via->host);
   if (inet_aton(via->host,&addr_via) == 0) {
      /* need name resolution */
      sts=get_ip_by_host(via->host, &addr_via);
      if (sts == STS_FAILURE) {
         DEBUGC(DBCLASS_DNS, "is_via_local: cannot resolve VIA [%s]",
                via->host);
         return STS_FAILURE;
      }
   }   

   found=0;
   for (i=0; ; i++) {
      /*
       * try to search by interface name first
       */
      ptr=my_interfaces[i];
      if (ptr==(char*)-1) break; /* end of list mark */

      if (ptr) {
         DEBUGC(DBCLASS_BABBLE,"resolving IP of interface %s",ptr);
         sts = get_ip_by_ifname(ptr, &addr_myself);
      }

      /* check the extracted VIA against my own host addresses */
      if (via->port) port=atoi(via->port);
      else port=SIP_PORT;

      if ( (memcmp(&addr_myself, &addr_via, sizeof(addr_myself))==0) &&
           (port == configuration.sip_listen_port) ) {
         DEBUG("address match [%s] <-> [%s]", inet_ntoa(addr_myself),
               inet_ntoa(addr_via));
         found=1;
	 break;
      }
   }

   return (found)? STS_TRUE : STS_FALSE;
}


/*
 * compares two URLs
 * (by now, only hostname and username are compared)
 *
 * RETURNS
 *	STS_SUCCESS if equal
 *	STS_FAILURE if non equal or error
 */
int compare_url(osip_uri_t *url1, osip_uri_t *url2) {
   int sts;
   struct in_addr addr1, addr2;

   /* sanity checks */
   if ((url1 == NULL) || (url2 == NULL)) {
      ERROR("compare_url: NULL ptr: url1=0x%p, url2=0x%p",url1, url2);
      return STS_FAILURE;
   }

   /* sanity checks: host part is a MUST */
   if ((url1->host == NULL) || (url2->host == NULL)) {
      ERROR("compare_url: NULL ptr: url1->host=0x%p, url2->host=0x%p",
            url1->host, url2->host);
      return STS_FAILURE;
   }

   /* get the IP addresses from the (possible) hostnames */
   sts=get_ip_by_host(url1->host, &addr1);
   if (sts == STS_FAILURE) {
      DEBUGC(DBCLASS_PROXY, "compare_url: cannot resolve host [%s]",
             url1->host);
      return STS_FAILURE;
   }

   sts=get_ip_by_host(url2->host, &addr2);
   if (sts == STS_FAILURE) {
      DEBUGC(DBCLASS_PROXY, "compare_url: cannot resolve host [%s]",
             url2->host);
      return STS_FAILURE;
   }

   /* Broken(?) MSN messenger - does not supply a user name part.
      So we simply compare the host part then */
   if ((url1->username == NULL) || (url2->username == NULL)) {
/* let's be nice to Billy boy and don't complain evey time ;-)
//      WARN("compare_url: NULL username pointer: MSN messenger is known to "
//           "trigger this one!"); */
      DEBUGC(DBCLASS_DNS, "comparing broken urls (no user): "
            "%s[%s] -> %s[%s]",
            url1->host, inet_ntoa(addr1), url2->host, inet_ntoa(addr2));
      if (memcmp(&addr1, &addr2, sizeof(addr1))==0) {
         sts = STS_SUCCESS;
      } else {
         sts = STS_FAILURE;
      }
      return sts;
   }

   /* we have a proper URL */
   /* comparison of hosts should be based on IP addresses, no? */
   DEBUGC(DBCLASS_DNS, "comparing urls: %s@%s[%s] -> %s@%s[%s]",
         url1->username, url1->host, inet_ntoa(addr1),
         url2->username, url2->host, inet_ntoa(addr2));
   if ((strcmp(url1->username, url2->username)==0) &&
       (memcmp(&addr1, &addr2, sizeof(addr1))==0)) {
      sts = STS_SUCCESS;
   } else {
      sts = STS_FAILURE;
   }

   return sts;
}


/*
 * check if a given request is addressed to local. I.e. it is addressed
 * to the porxy itself (IP of my inbound or outbound interface, same port)
 *
 * RETURNS
 *	STS_TRUE if the request is addressed local
 *	STS_FALSE otherwise
 */
int is_sipuri_local (osip_message_t *sip) {
   int sts, found;
   struct in_addr addr_uri, addr_myself;
   char *my_interfaces[]=
        { configuration.inbound_if,  configuration.outbound_if,  (char*)-1 };
   int port;
   int i;
   char *ptr;

   if (sip==NULL) {
      ERROR("called is_sipuri_local with NULL sip");
      return STS_FALSE;
   }

   if (!sip || !sip->req_uri) {
      ERROR("is_sipuri_local: no request URI present");
      return STS_FALSE;
   }

   DEBUGC(DBCLASS_DNS,"check for local SIP URI %s:%s",
          sip->req_uri->host? sip->req_uri->host : "*NULL*",
          sip->req_uri->port? sip->req_uri->port : "*NULL*");

   if (inet_aton(sip->req_uri->host, &addr_uri) == 0) {
      /* need name resolution */
      get_ip_by_host(sip->req_uri->host, &addr_uri);
   }   

   found=0;
   for (i=0; ; i++) {
      /*
       * try to search by interface name first
       */
      ptr=my_interfaces[i];
      if (ptr==(char*)-1) break; /* end of list mark */

      if (ptr) {
         DEBUGC(DBCLASS_BABBLE,"resolving IP of interface %s",ptr);
         sts = get_ip_by_ifname(ptr, &addr_myself);
      }

      /* check the extracted HOST against my own host addresses */
      if (sip->req_uri->port) {
         port=atoi(sip->req_uri->port);
      } else {
         port=SIP_PORT;
      }

      if ( (memcmp(&addr_myself, &addr_uri, sizeof(addr_myself))==0) &&
           (port == configuration.sip_listen_port) ) {
         DEBUG("address match [%s] <-> [%s]", inet_ntoa(addr_myself),
               inet_ntoa(addr_uri));
         found=1;
	 break;
      }
   }

   DEBUGC(DBCLASS_DNS, "SIP URI is %slocal", found? "":"not ");
   return (found)? STS_TRUE : STS_FALSE;
}


/*
 * check if a given request (outbound -> inbound) shall its
 * request URI get rewritten based upon our UA knowledge
 *
 * RETURNS
 *	STS_TRUE if to be rewritten
 *	STS_FALSE otherwise
 */
int check_rewrite_rq_uri (osip_message_t *sip) {
   int i, j, sts;
   int dflidx;
   osip_header_t *ua_hdr;

   /* get index of default entry */
   dflidx=(sizeof(RQ_rewrite)/sizeof(RQ_rewrite[0])) - 1;

   /* check fort existence of method */
   if ((sip==NULL) ||
       (sip->sip_method==NULL)) {
      ERROR("check_rewrite_rq_uri: got NULL method");
      return STS_FALSE;
   }

   /* extract UA string */
   osip_message_get_user_agent (sip, 0, &ua_hdr);
   if ((ua_hdr==NULL) || (ua_hdr->hvalue==NULL)) {
      WARN("check_rewrite_rq_uri: NULL UA in Header, using default");
      i=dflidx;
   } else {
      /* loop through the knowledge base */
      for (i=0; RQ_rewrite[i].UAstring; i++) {
         if (strncmp(RQ_rewrite[i].UAstring, ua_hdr->hvalue,
                    sizeof(RQ_rewrite[i].UAstring))==0) {
            DEBUGC(DBCLASS_SIP, "got knowledge entry for [%s]",
                   ua_hdr->hvalue);
            break;
         }
      } /* for i */
   } /* if ua_hdr */

   for (j=0; RQ_method[j].name; j++) {
      if (strncmp(RQ_method[j].name,
                 sip->sip_method, RQ_method[j].size)==0) {
         if (RQ_rewrite[i].action[j] >= 0) {
            sts = (RQ_rewrite[i].action[j])? STS_TRUE: STS_FALSE;
         } else {
	    sts = (RQ_rewrite[dflidx].action[j])? STS_TRUE: STS_FALSE;
         }
         DEBUGC(DBCLASS_SIP, "check_rewrite_rq_uri: [%s:%s, i=%i, j=%i] "
                "got action %s",
                (sip && sip->sip_method) ?
                  sip->sip_method : "*NULL*",
                (ua_hdr && ua_hdr->hvalue)? ua_hdr->hvalue:"*NULL*",
                 i, j, (sts==STS_TRUE)? "rewrite":"norewrite");
         return sts;
      }
   } /* for j */

   WARN("check_rewrite_rq_uri: didn't get a hit of the method [%s]",
        sip->sip_method);
   return STS_FALSE;
}


/*
 * PROXY_GEN_RESPONSE
 *
 * send an proxy generated response back to the client.
 * Only errors are reported from the proxy itself.
 *  code =  SIP result code to deliver
 *
 * RETURNS
 *	STS_SUCCESS on success
 *	STS_FAILURE on error
 */
int sip_gen_response(osip_message_t *request, int code) {
   osip_message_t *response;
   int sts;
   osip_via_t *via;
   int port;
   char *buffer;
   struct in_addr addr;

   /* create the response template */
   if ((response=msg_make_template_reply(request, code))==NULL) {
      ERROR("proxy_response: error in msg_make_template_reply");
      return STS_FAILURE;
   }

   /* we must check if first via has x.x.x.x address. If not, we must resolve it */
   osip_message_get_via (response, 0, &via);
   if (via == NULL)
   {
      ERROR("proxy_response: Cannot send response - no via field");
      return STS_FAILURE;
   }


   /* name resolution */
   if (inet_aton(via->host, &addr) == 0)
   {
      /* need name resolution */
      DEBUGC(DBCLASS_DNS,"resolving name:%s",via->host);
      sts = get_ip_by_host(via->host, &addr);
      if (sts == STS_FAILURE) {
         DEBUGC(DBCLASS_PROXY, "sip_gen_response: cannot resolve via [%s]",
                via->host);
         return STS_FAILURE;
      }
   }   

   sts = osip_message_to_str(response, &buffer);
   if (sts != 0) {
      ERROR("proxy_response: msg_2char failed");
      return STS_FAILURE;
   }


   if (via->port) {
      port=atoi(via->port);
   } else {
      port=SIP_PORT;
   }

   /* send to destination */
   sipsock_send_udp(&sip_socket, addr, port,
                    buffer, strlen(buffer), 1);

   /* free the resources */
   osip_message_free(response);
   osip_free(buffer);
   return STS_SUCCESS;
}


/*
 * PROXY_ADD_MYVIA
 *
 * interface == IF_OUTBOUND, IF_INBOUND
 *
 * RETURNS
 *	STS_SUCCESS on success
 *	STS_FAILURE on error
 */
int sip_add_myvia (osip_message_t *request, int interface) {
   struct in_addr addr;
   char tmp[URL_STRING_SIZE];
   osip_via_t *via;
   int sts;

   if (interface == IF_OUTBOUND) {
      sts = get_ip_by_ifname(configuration.outbound_if, &addr);
      if (sts == STS_FAILURE) {
         ERROR("can't find outbound interface %s - configuration error?",
               configuration.outbound_if);
         return STS_FAILURE;
      }
   } else {
      sts = get_ip_by_ifname(configuration.inbound_if, &addr);
      if (sts == STS_FAILURE) {
         ERROR("can't find inbound interface %s - configuration error?",
               configuration.inbound_if);
         return STS_FAILURE;
      }
   }

   sprintf(tmp, "SIP/2.0/UDP %s:%i", inet_ntoa(addr),
           configuration.sip_listen_port);
   DEBUGC(DBCLASS_BABBLE,"adding VIA:%s",tmp);

   sts = osip_via_init(&via);
   if (sts!=0) return STS_FAILURE; /* allocation failed */

   sts = osip_via_parse(via, tmp);
   if (sts!=0) return STS_FAILURE;

   osip_list_add(request->vias,via,0);

   return STS_SUCCESS;
}


/*
 * PROXY_DEL_MYVIA
 *
 * RETURNS
 *	STS_SUCCESS on success
 *	STS_FAILURE on error
 */
int sip_del_myvia (osip_message_t *response) {
   osip_via_t *via;
   int sts;

   DEBUGC(DBCLASS_PROXY,"deleting topmost VIA");
   via = osip_list_get (response->vias, 0);
   
   if ( is_via_local(via) == STS_FALSE ) {
      ERROR("I'm trying to delete a VIA but it's not mine! host=%s",via->host);
      return STS_FAILURE;
   }

   sts = osip_list_remove(response->vias, 0);
   osip_via_free (via);
   return STS_SUCCESS;
}


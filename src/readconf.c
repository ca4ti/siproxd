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

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <netinet/in.h>

#include <osip/smsg.h>

#include "siproxd.h"
#include "log.h"

static char const ident[]="$Id: " __FILE__ ": " PACKAGE "-" VERSION "-"\
			  BUILDSTR " $";

/* configuration storage */
extern struct siproxd_config configuration;

/* prototypes used locally only */
static int parse_config (FILE *configfile);


/* try to open (witchever found first):
 *	<name>
 *	$HOME/.<name>rc
 *	/etc/<name>.conf
 *	/usr/etc/<name>.conf
 *	/usr/local/etc/<name>.conf
 *
 * RETURNS
 *	STS_SUCCESS on success
 *	STS_FAILURE on error
 */
int read_config(char *name, int search) {
   int sts;
   FILE *configfile=NULL;
   int i;
   char tmp[256];
   const char *completion[] = {
	"%s/.%src",		/* this one is special... (idx=0)*/
	"/etc/%s.conf",
	"/usr/etc/%s.conf",
	"/usr/local/etc/%s.conf",
	NULL };


   DEBUGC(DBCLASS_CONFIG,"trying to read config file");

   /* shall I search the config file myself ? */
   if (search != 0) {
      /* yup, try to find it */
      for (i=0; completion[i]!=NULL; i++) {
	 switch (i) {
	 case 0:
            sprintf(tmp,completion[i],getenv("HOME"),name);
	    break;
	 default:
            sprintf(tmp,completion[i],name);
	    break;
	 }
	 DEBUGC(DBCLASS_CONFIG,"... trying %s",tmp);
         configfile = fopen(tmp,"r");
	 if (configfile==NULL) continue;
	 break; /* got config file */
      }
   } else {
         /* don't search it, just try the one given file */
	 DEBUGC(DBCLASS_CONFIG,"... trying %s",name);
         configfile = fopen(name,"r");
   }

   /* config file not found or unable to open for read */
   if (configfile==NULL) {
      ERROR ("could not open config file: %s", strerror(errno));
      return STS_FAILURE;
   }

   sts = parse_config(configfile);
   fclose(configfile);
   return sts;
}


/*
 * parse configuration file
 *
 * RETURNS
 *	STS_SUCCESS on success
 *	STS_FAILURE on error
 */
static int parse_config (FILE *configfile) {
   char buff[128];
   char *ptr;
   int i;
   int j;
   int num;

   struct cfgopts {
      char *keyword;
      enum type {TYP_INT4, TYP_STRING, TYP_FLOAT} type;
      void *dest;
   } configoptions[] = {
      { "debug_level",         TYP_INT4,   &configuration.debuglevel },
      { "sip_listen_port",     TYP_INT4,   &configuration.sip_listen_port },
      { "daemonize",           TYP_INT4,   &configuration.daemonize },
      { "host_inbound",        TYP_STRING, &configuration.inboundhost },
      { "host_outbound",       TYP_STRING, &configuration.outboundhost },
      { "rtp_port_low",        TYP_INT4,   &configuration.rtp_port_low },
      { "rtp_port_high",       TYP_INT4,   &configuration.rtp_port_high },
      { "rtp_timeout",         TYP_INT4,   &configuration.rtp_timeout },
      { "rtp_proxy_enable",    TYP_INT4,   &configuration.rtp_proxy_enable },
      { "user",                TYP_STRING, &configuration.user },
      { "chrootjail",          TYP_STRING, &configuration.chrootjail },
      { "hosts_allow_reg",     TYP_STRING, &configuration.hosts_allow_reg },
      { "hosts_allow_sip",     TYP_STRING, &configuration.hosts_allow_sip },
      { "hosts_deny_sip",      TYP_STRING, &configuration.hosts_deny_sip },
      { "hosts_deny_sip",      TYP_STRING, &configuration.hosts_deny_sip },
      { "proxy_auth_realm",    TYP_STRING, &configuration.proxy_auth_realm },
      { "proxy_auth_passwd",   TYP_STRING, &configuration.proxy_auth_passwd },
      { "proxy_auth_pwfile",   TYP_STRING, &configuration.proxy_auth_pwfile },
      {0, 0, 0}
   };


   while (fgets(buff,sizeof(buff),configfile) != NULL) {
      /* life insurance */
      buff[sizeof(buff)-1]='\0';

      /* strip newline if present */
      if (buff[strlen(buff)-1]=='\n') buff[strlen(buff)-1]='\0';

      /* strip emty lines */
      if (strlen(buff) == 0) continue;

      /* strip comments and line with only whitespaces */
      for (i=0;i<strlen(buff);i++) {
         if ((buff[i] == ' ') && (buff[i] == '\t')) continue;
         if (buff[i] =='#') i=strlen(buff);
         break;
      }
      if (i == strlen(buff)) continue;

      DEBUGC(DBCLASS_CONFIG,"pc:\"%s\"",buff);

      /* scan for known keyword */
      for (j=0; configoptions[j].keyword != NULL; j++) {
         if ((ptr=strstr(buff, configoptions[j].keyword)) != NULL) {
            ptr += strlen(configoptions[j].keyword);
            DEBUGC(DBCLASS_CONFIG,"got keyword:\"%s\"",
	                          configoptions[j].keyword);

	    /* check for argument separated by '=' */
            if ((ptr=strchr(ptr,'=')) == NULL) {;
	       ERROR("argument missing to config parameter %s",
	             configoptions[j].keyword);
	       break;
            }
	    do {ptr++;} while (*ptr == ' '); /* skip spaces after '=' */
            
            DEBUGC(DBCLASS_CONFIG,"got argument:\"%s\"",ptr);

	    num=0;
	    switch (configoptions[j].type) {
	    case TYP_INT4:
	         num=sscanf(ptr,"%i",(int*)configoptions[j].dest);
                 DEBUGC(DBCLASS_BABBLE,"INT4=%i",*(int*)configoptions[j].dest);
	      break;	    

	    case TYP_STRING:
//	         num=sscanf(ptr,"%a[^#]",(char**)configoptions[j].dest);
	         num=sscanf(ptr,"%as",(char**)configoptions[j].dest);
                 DEBUGC(DBCLASS_BABBLE,"STRING=%s",*(char**)configoptions[j].dest);
	      break;	    

	    default:
	      break;
	    }
	    if (num == 0) {
	       ERROR("illegal format in config file, line:\"%s\"",buff);
	    }

            break;
	 }
      }
   }

   return STS_SUCCESS;
}
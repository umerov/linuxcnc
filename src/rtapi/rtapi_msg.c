/********************************************************************
* Description:  rtapi_msg.c
*               This file, 'rtapi_msg.c', implements the messaging
*               functions for both kernel and userland thread
*               systems.  See rtapi.h for more info.
********************************************************************/

#include "config.h"
#include "rtapi.h"
#include "rtapi_common.h"

#ifdef MODULE
#    include <linux/module.h>	/* EXPORT_SYMBOL */
#    include <linux/kernel.h>	/* kernel's vsnprintf */
#    define RTPRINTBUFFERLEN 1024
#else  /* user land */
#    include <stdio.h>		/* libc's vsnprintf() */
#endif

static int msg_level = RTAPI_MSG_INFO;	/* message printing level */ //XXX

// most RT systems use printk()
#ifndef PRINTK
#define PRINTK printk
#endif


#ifdef MODULE
void default_rtapi_msg_handler(msg_level_t level, const char *fmt,
			      va_list ap) {
    char buf[RTPRINTBUFFERLEN];
    vsnprintf(buf, RTPRINTBUFFERLEN, fmt, ap);
    PRINTK(buf);
}

#else /* user land */
void default_rtapi_msg_handler(msg_level_t level, const char *fmt,
			       va_list ap) {
    if(level == RTAPI_MSG_ALL)
	vfprintf(stdout, fmt, ap);
    else
	vfprintf(stderr, fmt, ap);
}
#endif

static rtapi_msg_handler_t rtapi_msg_handler = default_rtapi_msg_handler;

#ifdef RTAPI

rtapi_msg_handler_t rtapi_get_msg_handler(void) {
    return rtapi_msg_handler;
}

void rtapi_set_msg_handler(rtapi_msg_handler_t handler) {
    if (handler == NULL)
	rtapi_msg_handler = default_rtapi_msg_handler;
    else
	rtapi_msg_handler = handler;
}
#endif  /* RTAPI */

void rtapi_print(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    rtapi_msg_handler(RTAPI_MSG_ALL, fmt, args);
    va_end(args);
}


void rtapi_print_msg(int level, const char *fmt, ...)
{
    va_list args;

    if ((level <= msg_level) && (msg_level != RTAPI_MSG_NONE)) {
	va_start(args, fmt);
	rtapi_msg_handler(level, fmt, args);
	va_end(args);
    }
}

int rtapi_snprintf(char *buf, unsigned long int size,
		   const char *fmt, ...) {
    va_list args;
    int result;

    va_start(args, fmt);
    result = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return result;
}

int rtapi_vsnprintf(char *buf, unsigned long int size, const char *fmt,
		    va_list ap) {
    return vsnprintf(buf, size, fmt, ap);
}

int rtapi_set_msg_level(int level) {
    if ((level < RTAPI_MSG_NONE) || (level > RTAPI_MSG_ALL)) {
	return -EINVAL;
    }
    msg_level = level;
    return 0;
}

int rtapi_get_msg_level() { 
    return msg_level;
}

#ifdef MODULE
EXPORT_SYMBOL(rtapi_snprintf);
EXPORT_SYMBOL(rtapi_vsnprintf);
EXPORT_SYMBOL(rtapi_print);
EXPORT_SYMBOL(rtapi_print_msg);
EXPORT_SYMBOL(rtapi_set_msg_level);
EXPORT_SYMBOL(rtapi_get_msg_level);
EXPORT_SYMBOL(rtapi_set_msg_handler);
EXPORT_SYMBOL(rtapi_get_msg_handler);
#endif
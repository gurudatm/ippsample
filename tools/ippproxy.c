/*
 * IPP Proxy implementation for HP PCL and IPP Everywhere printers.
 *
 * Copyright © 2016-2022 by the Printer Working Group.
 * Copyright © 2014-2018 by Apple Inc.
 *
 * Licensed under Apache License v2.0.  See the file "LICENSE" for more
 * information.
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#ifndef _WIN32
#  include <signal.h>
#  include <unistd.h>
#  include <fcntl.h>
#endif // !_WIN32
#include <cups/cups.h>
#include <cups/thread.h>


//
// Macros to implement a simple Fibonacci sequence for variable back-off...
//

#define FIB_NEXT(v) (((((v >> 8) + (v & 255) - 1) % 60) + 1) | ((v & 255) << 8))
#define FIB_VALUE(v) (v & 255)


/*
 * Local types...
 */

typedef struct proxy_info_s		/* Proxy thread information */
{
  int		done;			/* Non-zero when done */
  http_t	*http;			/* Connection to Infrastructure Printer */
  char		resource[256];		/* Resource path */
  const char	*printer_uri,		/* Infrastructure Printer URI */
          *notifications_uri,		/* Universal Print Support, Notifications URI */
		*device_uri,		/* Output device URI */
		*device_uuid,		/* Output device UUID */
		*outformat;		/* Desired output format (NULL for auto) */
  ipp_t		*device_attrs;		/* Output device attributes */
} proxy_info_t;

typedef struct proxy_job_s		/* Proxy job information */
{
  ipp_jstate_t	local_job_state;	/* Local job-state value */
  int		local_job_id,		/* Local job-id value */
		remote_job_id,		/* Remote job-id value */
		remote_job_state;	/* Remote job-state value */
} proxy_job_t;


/*
 * Local globals...
 */

static cups_thread_t	jobs_thread;	/* Job proxy processing thread */
static cups_thread_t	confUpdate_thread;	/* Job proxy processing thread */
static cups_array_t	*jobs_lst;		/* Local jobs */
#ifdef _WIN32
static cups_cond_t	jobs_cond = { CUPS_COND_INITIALIZER };
#else
static cups_cond_t	jobs_cond = CUPS_COND_INITIALIZER;
#endif
					/* Condition variable to signal changes */
static cups_mutex_t	jobs_mutex = CUPS_MUTEX_INITIALIZER;
					/* Mutex for condition variable */
static cups_rwlock_t	jobs_rwlock = CUPS_RWLOCK_INITIALIZER;
					/* Read/write lock for jobs array */
static char		*password = NULL;
					/* Password, if any */

static const char * const printer_attrs[] =
		{			/* Printer attributes we care about */

    "charset-configured",
    "charset-supported",
    //"compression-supported",
    "copies-default",
    "copies-supported",
    "document-format-default",
    "document-format-supported",
    "document-format-preferred",
    "document-format-details-supported",
    "job-pages-per-set-supported",
    "finishings-default",
    "finishings-supported",
    "generated-natural-language-supported", //Fiery Not Supported
    "ipp-features-supported", //Fiery Not Supported
    "ipp-versions-supported",
    "jpeg-k-octets-supported", //Fiery Not Supported
    "media-bottom-margin-supported",
    "media-col-database",
    "media-col-default",
    "media-col-ready",
    "media-col-supported",
    "media-default",
    "media-left-margin-supported",
    "media-ready",
    "media-right-margin-supported",
    "media-size-supported",
    "media-source-supported",
    "media-supported",
    "media-supported",
    "media-top-margin-supported",
    "media-type-supported",
    "multiple-document-handling-default",
    "multiple-document-handling-supported",
    "multiple-document-jobs-supported",
    "natural-language-configured",
    "number-up-default",
    "number-up-supported",
    "operations-supported",
    "orientation-requested-default",
    "orientation-requested-supported",
    "output-bin-default",
    "output-bin-supported",
    "pdf-fit-to-page-default", //Fiery Not Supported
    "pdf-k-octets-supported",
    "pdf-size-constraints", //Fiery Not Supported
    "pdf-versions-supported", //Fiery Not Supported
    "presentation-direction-number-up-default", //Fiery Not Supported
    "presentation-direction-number-up-supported", //Fiery Not Supported
    "print-color-mode-default",
    "print-color-mode-supported",
    "print-quality-default",
    "print-quality-supported",
    "print-scaling-default", //Fiery Not Supported
    "print-scaling-supported", //Fiery Not Supported
    "printer-is-accepting-jobs",
    "printer-location",
    "printer-make-and-model",
    "printer-more-info",
    "printer-resolution-default",
    "printer-resolution-supported",
    "printer-state",
    "printer-state-message",
    "printer-state-reasons",
    "printer-up-time",
    "pwg-raster-document-resolution-supported", //Fiery Not Supported
    "pwg-raster-document-sheet-back", //Fiery Not Supported
    "pwg-raster-document-type-supported", //Fiery Not Supported
    "queued-job-count",
    "sides-default",
    "sides-supported",
    "urf-supported", //Fiery Not Supported
    "uri-authentication-supported",
    "uri-security-supported"
		};


static ipp_op_t const notification_op_list[] =
{
    IPP_OP_CREATE_PRINTER_SUBSCRIPTIONS,
    IPP_OP_CREATE_JOB_SUBSCRIPTIONS,
    IPP_OP_GET_NOTIFICATIONS,
    IPP_OP_SEND_NOTIFICATIONS
};
static int	stop_running = 0;
static int	verbosity = 0;
static int  reset_connection_count = 0;
int MAX_RESET_WAIT_COUNT = 720;

static const int local_pending_jobs[10];
//static const ipp_jstate_t local_pending_job_states[10];
static const int local_pending_job_states[10];

/*
 * Local functions...
 */

static int is_a_notification_operation(ipp_op_t op);
static void	acknowledge_identify_printer(http_t *http, const char *printer_uri, const char *notifications_uri, const char *resource, const char *device_uuid);
static bool	attrs_are_equal(ipp_attribute_t *a, ipp_attribute_t *b);
static int	compare_jobs(proxy_job_t *a, proxy_job_t *b);
static ipp_t	*create_media_col(const char *media, const char *source, const char *type, int width, int length, int margins);
static ipp_t	*create_media_size(int width, int length);
static void	deregister_printer(http_t *http, const char *printer_uri,const char *notifications_uri ,  const char *resource, int subscription_id, const char *device_uuid);
static proxy_job_t *find_job(int remote_job_id);
static ipp_t	*get_device_attrs(const char *device_uri);
static void	make_uuid(const char *device_uri, char *uuid, size_t uuidsize);
static const char *password_cb(const char *prompt, http_t *http, const char *method, const char *resource, void *user_data);
static void	plogf(proxy_job_t *pjob, const char *message, ...);
static void	*proxy_jobs(proxy_info_t *info);
static int	send_printer_Details(http_t *http, const char *printer_uri,const char *notifications_uri ,  const char *resource, const char *device_uri, const char *device_uuid);
static int	register_printer(http_t *http, const char *printer_uri, const char *resource, const char *device_uri, const char *device_uuid);
static void	run_job(proxy_info_t *info, proxy_job_t *pjob);
static void	run_printer(http_t *http, http_t *notifications_http, const char* printer_uri, const char *notifications_uri ,  const char *resource, int subscription_id, const char *device_uri, const char *device_uuid, const char *outformat);
static void	send_document(proxy_info_t *info, proxy_job_t *pjob, ipp_t *job_attrs, ipp_t *doc_attrs, int doc_number);
static void	sighandler(int sig);
static int	update_device_attrs(http_t *http, const char *printer_uri, const char *resource, const char *device_uuid, ipp_t *old_attrs, ipp_t *new_attrs);
static void update_attributes(http_t* http, const char* printer_uri, const char* resource, int subscription_id, const char* device_uri, const char* device_uuid, const char* outformat);
static void	update_document_status(proxy_info_t *info, proxy_job_t *pjob, int doc_number, ipp_dstate_t doc_state);
static void	update_job_status(proxy_info_t *info, proxy_job_t *pjob);
static void* create_printer_subscriptions(proxy_info_t *info, proxy_job_t *pjob);
static void* create_job_subscriptions(proxy_info_t *info, proxy_job_t *pjob);
static void* create_job_subscriptions(proxy_info_t *info, proxy_job_t *pjob);
static void* process_pending_in_queue_jobs(proxy_info_t *info, proxy_job_t *pjob);
static void* Update_active_jobs(proxy_info_t *info, proxy_job_t *pjob);
static void* print_conn_info(ipp_t* http,char* func);
static bool	update_remote_jobs(http_t *http, const char *printer_uri, const char *resource, const char *device_uuid);
static void	usage(int status);
static int cupsGetJobs_proxy( http_t* http, cups_job_t** jobs, const char* resource, const char* printer_uri, const char* device_uuid);


/*
 * 'main()' - Main entry for ippproxy.
 */

int					/* O - Exit status */
main(int  argc,				/* I - Number of command-line arguments */
     char *argv[])			/* I - Command-line arguments */
{
  int		i;			/* Looping var */
  char		*opt,			/* Current option */
		*device_uri = NULL,	/* Device URI */
		*printer_uri = NULL,	/* Infrastructure printer URI */
                *notifications_uri = NULL;/* Infrastructure printer URI */
  cups_dest_t	*dest;			/* Destination for printer URI */
    http_t	*printer_http, *notification_http;			/* Connection to printer */
  char		resource[1024];		/* Resource path */
  int		subscription_id;	/* Event subscription ID */
  char		device_uuid[46];	/* Device UUID URN */
  const char	*outformat = NULL;	/* Output format */
  unsigned	interval = 1;		// Current retry interval


 /*
  * Parse command-line...
  */

  for (i = 1; i < argc; i ++)
  {
    if (argv[i][0] == '-' && argv[i][1] == '-')
    {
      if (!strcmp(argv[i], "--help"))
      {
        usage(0);
      }
      else if (!strcmp(argv[i], "--version"))
      {
        puts(IPPSAMPLE_VERSION);
        return (0);
      }
      else
      {
        fprintf(stderr, "ippproxy: Unknown option '%s'.\n", argv[i]);
        usage(1);
      }
    }
    else if (argv[i][0] == '-')
    {
      for (opt = argv[i] + 1; *opt; opt ++)
      {
        switch (*opt)
	{
	  case 'd' : /* -d device-uri */
	      i ++;
	      if (i >= argc)
	      {
	        fputs("ippproxy: Missing device URI after '-d' option.\n", stderr);
		usage(1);
	      }

	      if (strncmp(argv[i], "ipp://", 6) && strncmp(argv[i], "ipps://", 7) && strncmp(argv[i], "socket://", 9))
	      {
	        fputs("ippproxy: Unsupported device URI scheme.\n", stderr);
	        usage(1);
	      }

	      device_uri = argv[i];
	      break;

          case 'm' : /* -m mime/type */
              i ++;
              if (i >= argc)
	      {
	        fputs("ippproxy: Missing MIME media type after '-m' option.\n", stderr);
	        usage(1);
	      }

	      outformat = argv[i];
	      break;
                    case 'n' : /* -n notification-uri*/
                        i ++;
                        if (i >= argc)
                        {
                            fputs("ippproxy: Missing notifications URI after '-n' option.\n", stderr);
                            usage(1);
                        }

                        if (strncmp(argv[i], "https://notification.print.microsoft.com", 40))
                        {
				if (strncmp(argv[i], "ipps://notification.print.microsoft.com", 39))
				{
					fputs("ippproxy: Unsupported notification URI scheme.\n", stderr);
					usage(1);
				} 
			}
                        notifications_uri = argv[i];
                        break;
	  case 'p' : /* -p password */
	      i ++;
	      if (i >= argc)
	      {
	        fputs("ippproxy: Missing password after '-p' option.\n", stderr);
		usage(1);
	      }

	      password = argv[i];
	      break;

	  case 'u' : /* -u user */
	      i ++;
	      if (i >= argc)
	      {
	        fputs("ippproxy: Missing username after '-u' option.\n", stderr);
		usage(1);
	      }

	      cupsSetUser(argv[i]);
	      break;

          case 'v' : /* Be verbose */
              verbosity ++;
              break;

	  default :
	      fprintf(stderr, "ippproxy: Unknown option '-%c'.\n", *opt);
	      usage(1);
	      break;
	}
      }
    }
    else if (printer_uri)
    {
      fprintf(stderr, "ippproxy: Unexpected option '%s'.\n", argv[i]);
      usage(1);
    }
    else
      printer_uri = argv[i];
  }

  if (!printer_uri)
    usage(1);

  if (!device_uri)
  {
    fputs("ippproxy: Must specify '-d device-uri'.\n", stderr);
    usage(1);
  }

  //if (!password)
  //password = getenv("IPPPROXY_PASSWORD");

  if (password)
    cupsSetPasswordCB(password_cb, password);

  //make_uuid(device_uri, device_uuid, sizeof(device_uuid));

    char* prefix_deviceuuid = "urn:uuid:";
    char* last = strrchr(printer_uri, '/');
    if (last) {
#ifdef _WIN32
        strcpy_s(device_uuid, sizeof(device_uuid), prefix_deviceuuid);
        strcat_s(device_uuid, sizeof(device_uuid), last + 1);
#else 
        strncpy(device_uuid, prefix_deviceuuid, sizeof(device_uuid));
        strncat(device_uuid, last + 1, sizeof(device_uuid));
#endif
    }
    else {
        //error
        return 0;
    
    }
    //printer_http = doConnection(printer_ui);
    //notification_http = doConnection(notification_uri);
    //update_settings(printer_http, printer_uri);
    //subscribe_printer(notification_http, notification_uri);
    //process_jobs(printer_http, printer_uri);
    //get_job_notification(notification_http, notification_uri);
    //unsubscribe(notification_http, notification_uri)

 /*
  * Connect to the infrastructure printer...
  */

  //dest = cupsGetDestWithURI("infra", printer_uri);
  dest = cupsGetDestWithURI(NULL, printer_uri);

  if (verbosity){
    plogf(NULL, "Main thread connecting to '%s'.", printer_uri);
  }

    while ((printer_http = cupsConnectDest(dest, CUPS_DEST_FLAGS_DEVICE, 30000, NULL, resource, sizeof(resource), NULL, NULL)) == NULL)
  {
    interval = FIB_NEXT(interval);

    plogf(NULL, "'%s' is not responding, retrying in %u seconds.", printer_uri, FIB_VALUE(interval));
    sleep(FIB_VALUE(interval));
  }

  if (verbosity)
    plogf(NULL, "Connected to '%s'.", printer_uri);

  cupsFreeDests(1, dest);

    //dest = cupsGetDestWithURI("infra", notifications_uri);
    dest = cupsGetDestWithURI(NULL, notifications_uri);

    if (verbosity)
        plogf(NULL, "Main thread connecting to '%s'.", notifications_uri);


    while ((notification_http = cupsConnectDest(dest, CUPS_DEST_FLAGS_DEVICE, 30000, NULL, resource, sizeof(resource), NULL, NULL)) == NULL)
    {
    	interval = FIB_NEXT(interval);
        //int interval = 1 + (CUPS_RAND() % 30);
        /* Retry interval */

        plogf(NULL, "'%s' is not responding, retrying in %d seconds.", notifications_uri, interval);
	sleep(FIB_VALUE(interval));
    }

    if (verbosity)
        plogf(NULL, "Connected to '%s'.", notifications_uri);

    cupsFreeDests(1, dest);


 /*
  * Register the printer and wait for jobs to process...
  */

#ifndef _WIN32
  signal(SIGHUP, sighandler);
  signal(SIGINT, sighandler);
  signal(SIGTERM, sighandler);
#endif // !_WIN32

    if ((subscription_id = register_printer(notification_http, notifications_uri , resource, device_uri, device_uuid)) == 0)
  {
        httpClose(notification_http);
    return (1);
  }

  run_printer(printer_http,notification_http, printer_uri, notifications_uri ,  resource, subscription_id, device_uri, device_uuid, outformat);
  deregister_printer(printer_http, printer_uri,notifications_uri ,  resource, subscription_id, device_uuid);

	httpClose(printer_http);
    httpClose(notification_http);
  return (0);
}


static int is_a_notification_operation(ipp_op_t op){
    int n_size = sizeof(notification_op_list);
    for(int i = 0 ; i < n_size; i++){
        if(op == notification_op_list[i]){
            return 1;
        }
    }
    return 0;
}

/*
 * 'acknowledge_identify_printer()' - Acknowledge an Identify-Printer request.
 */
static void
acknowledge_identify_printer(
    http_t     *http,			/* I - HTTP connection */
    const char *printer_uri,		/* I - Printer URI */
    const char *notifications_uri ,     /* Universal Print Support, Notifications URI */
    const char *resource,		/* I - Resource path */
    const char *device_uuid)		/* I - Device UUID */
{
  ipp_t		*request,		/* IPP request */
		*response;		/* IPP response */
  ipp_attribute_t *actions,		/* "identify-actions" attribute */
		*message;		/* "message" attribute */


  request = ippNewRequest(IPP_OP_ACKNOWLEDGE_IDENTIFY_PRINTER);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, printer_uri);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "device-uuid", NULL, device_uuid);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());

  response = cupsDoRequest(http, request, resource);

  actions = ippFindAttribute(response, "identify-actions", IPP_TAG_KEYWORD);
  message = ippFindAttribute(response, "message", IPP_TAG_TEXT);

  if (ippContainsString(actions, "display"))
    printf("IDENTIFY-PRINTER: display (%s)\n", message ? ippGetString(message, 0, NULL) : "No message supplied");

  if (!actions || ippContainsString(actions, "sound"))
    puts("IDENTIFY-PRINTER: sound\007");

  ippDelete(response);
}


/*
 * 'attrs_are_equal()' - Compare two attributes for equality.
 */

static bool				/* O - `true` if equal, `false` otherwise */
attrs_are_equal(ipp_attribute_t *a,	/* I - First attribute */
                ipp_attribute_t *b)	/* I - Second attribute */
{
  size_t	i,			/* Looping var */
		count;			/* Number of values */
  ipp_tag_t	tag;			/* Type of value */


 /*
  * Check that both 'a' and 'b' point to something first...
  */

  if ((a != NULL) != (b != NULL))
    return (false);

  if (a == NULL && b == NULL)
    return (true);

 /*
  * Check that 'a' and 'b' are of the same type with the same number
  * of values...
  */

  if ((tag = ippGetValueTag(a)) != ippGetValueTag(b))
    return (false);

  if ((count = ippGetCount(a)) != ippGetCount(b))
    return (false);

 /*
  * Compare values...
  */

  switch (tag)
  {
    case IPP_TAG_INTEGER :
    case IPP_TAG_ENUM :
        for (i = 0; i < count; i ++)
        {
	  if (ippGetInteger(a, i) != ippGetInteger(b, i))
	    return (false);
	}
	break;

    case IPP_TAG_BOOLEAN :
        for (i = 0; i < count; i ++)
	{
	  if (ippGetBoolean(a, i) != ippGetBoolean(b, i))
	    return (false);
	}
	break;

    case IPP_TAG_KEYWORD :
        for (i = 0; i < count; i ++)
        {
	  if (strcmp(ippGetString(a, i, NULL), ippGetString(b, i, NULL)))
	    return (false);
	}
	break;

    default :
	return (false);
  }

 /*
  * If we get this far we must be the same...
  */

  return (true);
}


/*
 * 'compare_jobs()' - Compare two jobs.
 */

static int
compare_jobs(proxy_job_t *a,		/* I - First job */
             proxy_job_t *b)		/* I - Second job */
{
  return (a->remote_job_id - b->remote_job_id);
}


/*
 * 'create_media_col()' - Create a media-col value.
 */

static ipp_t *				/* O - media-col collection */
create_media_col(const char *media,	/* I - Media name */
		 const char *source,	/* I - Media source */
		 const char *type,	/* I - Media type */
		 int        width,	/* I - x-dimension in 2540ths */
		 int        length,	/* I - y-dimension in 2540ths */
		 int        margins)	/* I - Value for margins */
{
  ipp_t	*media_col = ippNew(),		/* media-col value */
	*media_size = create_media_size(width, length);
					/* media-size value */
  char	media_key[256];			/* media-key value */


  if (type && source)
    snprintf(media_key, sizeof(media_key), "%s_%s_%s%s", media, source, type, margins == 0 ? "_borderless" : "");
  else if (type)
    snprintf(media_key, sizeof(media_key), "%s__%s%s", media, type, margins == 0 ? "_borderless" : "");
  else if (source)
    snprintf(media_key, sizeof(media_key), "%s_%s%s", media, source, margins == 0 ? "_borderless" : "");
  else
    snprintf(media_key, sizeof(media_key), "%s%s", media, margins == 0 ? "_borderless" : "");

  ippAddString(media_col, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-key", NULL,
               media_key);
  ippAddCollection(media_col, IPP_TAG_PRINTER, "media-size", media_size);
  ippAddString(media_col, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-size-name", NULL, media);
  ippAddInteger(media_col, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                "media-bottom-margin", margins);
  ippAddInteger(media_col, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                "media-left-margin", margins);
  ippAddInteger(media_col, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                "media-right-margin", margins);
  ippAddInteger(media_col, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                "media-top-margin", margins);
  if (source)
    ippAddString(media_col, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-source", NULL, source);
  if (type)
    ippAddString(media_col, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-type", NULL, type);

  ippDelete(media_size);

  return (media_col);
}


/*
 * 'create_media_size()' - Create a media-size value.
 */

static ipp_t *				/* O - media-col collection */
create_media_size(int width,		/* I - x-dimension in 2540ths */
		  int length)		/* I - y-dimension in 2540ths */
{
  ipp_t	*media_size = ippNew();		/* media-size value */


  ippAddInteger(media_size, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "x-dimension",
                width);
  ippAddInteger(media_size, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "y-dimension",
                length);

  return (media_size);
}


/*
 * 'deregister_printer()' - Unregister the output device and cancel the printer subscription.
 */

static void
deregister_printer(
    http_t     *http,			/* I - Connection to printer */
    const char *printer_uri,		/* I - Printer URI */
        const char *notifications_uri , /* Universal Print Support, Notifications URI */
    const char *resource,		/* I - Resource path */
    int        subscription_id,		/* I - Subscription ID */
    const char *device_uuid)		/* I - Device UUID */
{
  ipp_t	*request;			/* IPP request */


 /*
  * Deregister the output device...
  */

  request = ippNewRequest(IPP_OP_DEREGISTER_OUTPUT_DEVICE);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, printer_uri);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL, device_uuid);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());

  ippDelete(cupsDoRequest(http, request, resource));

 /*
  * Then cancel the subscription we are using...
  */

  request = ippNewRequest(IPP_OP_CANCEL_SUBSCRIPTION);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, printer_uri);
  ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "notify-subscription-id", subscription_id);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());

  ippDelete(cupsDoRequest(http, request, resource));
}


/*
 * 'find_job()' - Find a remote job that has been queued for proxying...
 */

static proxy_job_t *			/* O - Proxy job or @code NULL@ if not found */
find_job(int remote_job_id)		/* I - Remote job ID */
{
  proxy_job_t	key,			/* Search key */
		*match;			/* Matching job, if any */


  key.remote_job_id = remote_job_id;

  cupsRWLockRead(&jobs_rwlock);
  match = (proxy_job_t *)cupsArrayFind(jobs_lst, &key);
  cupsRWUnlock(&jobs_rwlock);

  return (match);
}


/*
 * 'get_device_attrs()' - Get current attributes for a device.
 */

static ipp_t *				/* O - IPP attributes */
get_device_attrs(const char *device_uri)/* I - Device URI */
{
  ipp_t	*response = NULL;		/* IPP attributes */


  if (!strncmp(device_uri, "ipp://", 6) || !strncmp(device_uri, "ipps://", 7))
  {
   /*
    * Query the IPP printer...
    */

    size_t	i,			/* Looping var */
		count;			/* Number of values */
    cups_dest_t	*dest;			/* Destination for printer URI */
    http_t	*http;			/* Connection to printer */
    char	resource[1024];		/* Resource path */
    ipp_t	*request;		/* Get-Printer-Attributes request */
    ipp_attribute_t *urf_supported,	/* urf-supported */
		*pwg_supported;		/* pwg-raster-document-xxx-supported */
    unsigned	interval = 1;		// Current retry interval


   /*
    * Connect to the printer...
    */

    dest = cupsGetDestWithURI("device", device_uri);

    while ((http = cupsConnectDest(dest, CUPS_DEST_FLAGS_DEVICE, 30000, NULL, resource, sizeof(resource), NULL, NULL)) == NULL)
    {
      interval = FIB_NEXT(interval);

      plogf(NULL, "'%s' is not responding, retrying in %u seconds.", device_uri, FIB_VALUE(interval));
      sleep(FIB_VALUE(interval));
    }

    cupsFreeDests(1, dest);

   /*
    * Get the attributes...
    */

    request = ippNewRequest(IPP_OP_GET_PRINTER_ATTRIBUTES);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, device_uri);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());
    ippAddStrings(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", (int)(sizeof(printer_attrs) / sizeof(printer_attrs[0])), NULL, printer_attrs);

    response = cupsDoRequest(http, request, resource);

    if (cupsGetError() >= IPP_STATUS_ERROR_BAD_REQUEST)
    {
      fprintf(stderr, "ippproxy: Device at '%s' returned error: %s\n", device_uri, cupsGetErrorString());
      ippDelete(response);
      response = NULL;
    }

    httpClose(http);

   /*
    * Convert urf-supported to pwg-raster-document-xxx-supported, as needed...
    */

    urf_supported = ippFindAttribute(response, "urf-supported", IPP_TAG_KEYWORD);
    pwg_supported = ippFindAttribute(response, "pwg-raster-document-resolution-supported", IPP_TAG_RESOLUTION);
    if (urf_supported && !pwg_supported)
    {
      for (i = 0, count = ippGetCount(urf_supported); i < count; i ++)
      {
        const char *keyword = ippGetString(urf_supported, i, NULL);
					/* Value from urf_supported */

        if (!strncmp(keyword, "RS", 2))
        {
	  char	*ptr;			/* Pointer into value */
	  int	res;			/* Resolution */

          for (res = (int)strtol(keyword + 2, &ptr, 10); res > 0; res = (int)strtol(ptr + 1, &ptr, 10))
	  {
	    if (pwg_supported)
	      ippSetResolution(response, &pwg_supported, ippGetCount(pwg_supported), IPP_RES_PER_INCH, res, res);
	    else
	      pwg_supported = ippAddResolution(response, IPP_TAG_PRINTER, "pwg-raster-document-resolution-supported", IPP_RES_PER_INCH, res, res);
	  }
        }
      }
    }

    pwg_supported = ippFindAttribute(response, "pwg-raster-document-sheet-back", IPP_TAG_KEYWORD);
    if (urf_supported && !pwg_supported)
    {
      for (i = 0, count = ippGetCount(urf_supported); i < count; i ++)
      {
        const char *keyword = ippGetString(urf_supported, i, NULL);
					/* Value from urf_supported */

        if (!strncmp(keyword, "DM", 2))
        {
          if (!strcmp(keyword, "DM1"))
            pwg_supported = ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "pwg-raster-document-sheet-back", NULL, "normal");
          else if (!strcmp(keyword, "DM2"))
            pwg_supported = ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "pwg-raster-document-sheet-back", NULL, "flipped");
          else if (!strcmp(keyword, "DM3"))
            pwg_supported = ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "pwg-raster-document-sheet-back", NULL, "rotated");
          else
            pwg_supported = ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "pwg-raster-document-sheet-back", NULL, "manual-tumble");
        }
      }
    }

    pwg_supported = ippFindAttribute(response, "pwg-raster-document-type-supported", IPP_TAG_KEYWORD);
    if (urf_supported && !pwg_supported)
    {
      for (i = 0, count = ippGetCount(urf_supported); i < count; i ++)
      {
        const char *keyword = ippGetString(urf_supported, i, NULL);
					/* Value from urf_supported */
        const char *pwg_keyword = NULL;	/* Value for pwg-raster-document-type-supported */

        if (!strcmp(keyword, "ADOBERGB24"))
          pwg_keyword = "adobe-rgb_8";
	else if (!strcmp(keyword, "ADOBERGB48"))
          pwg_keyword = "adobe-rgb_16";
	else if (!strcmp(keyword, "SRGB24"))
          pwg_keyword = "srgb_8";
	else if (!strcmp(keyword, "W8"))
          pwg_keyword = "sgray_8";
	else if (!strcmp(keyword, "W16"))
          pwg_keyword = "sgray_16";

        if (pwg_keyword)
        {
	  if (pwg_supported)
	    ippSetString(response, &pwg_supported, ippGetCount(pwg_supported), pwg_keyword);
	  else
	    pwg_supported = ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "pwg-raster-document-type-supported", NULL, pwg_keyword);
        }
      }
    }
  }
  else
  {
   /*
    * Must be a socket-based HP PCL laser printer, report just standard size
    * information...
    */

    size_t		i;		/* Looping var */
    ipp_attribute_t	*media_col_database,
					/* media-col-database value */
			*media_size_supported;
					/* media-size-supported value */
    ipp_t		*media_col;	/* media-col-default value */
    static const int media_col_sizes[][2] =
        {
            { 29700, 42000 },
    //iso_a3_297x420mm
            { 27940, 43180 },
    //na_ledger_11x17in
            { 10477, 24130 },
    //na_number-10_4.125x9.5in
            { 11010, 22010 },
    //custom_11010x22010mm
            { 22860, 27940 },
    //na_9x11_9x11in
            { 27940, 43180 },
    //na_ledger_11x17in
            { 21590, 27940 },
    //na_letter_8.5x11in
            { 21590, 27940 },
    //na_letter_8.5x11in
            { 11990, 23500 },
    //custom_11990x23500mm
            { 21590, 27940 },
    //na_letter_8.5x11in
            { 21590, 27940 },
    //na_letter_8.5x11in
            { 27940, 43180 },
    //na_ledger_11x17in
            { 21590, 27940 },
    //na_letter_8.5x11in
            { 21590, 27940 },
    //na_letter_8.5x11in
            { 21590, 35560 },
    //na_legal_8.5x14in
            { 30480, 45720 },
    //na_arch-b_12x18in
            { 21000, 29700 },
    //iso_a4_210x297mm
            { 21590, 27940 }
    //na_letter_8.5x11in
    };
    static const char * const media_col_supported[] =
    {					/* media-col-supported values */
      "media-bottom-margin",
      "media-left-margin",
      "media-right-margin",
      "media-size",
      "media-size-name",
      "media-top-margin"
    };
    static const char * const media_supported[] =
    {					/* Default media sizes */
      "na_letter_8.5x11in",		/* Letter */
      "na_legal_8.5x14in",		/* Legal */
      "iso_a4_210x297mm"			/* A4 */
    };
    static const int quality_supported[] =
    {					/* print-quality-supported values */
      IPP_QUALITY_DRAFT,
      IPP_QUALITY_NORMAL,
      IPP_QUALITY_HIGH
    };
    static const int resolution_supported[] =
    {					/* printer-resolution-supported values */
      300,
      600
    };
    static const char * const sides_supported[] =
    {					/* sides-supported values */
      "one-sided",
      "two-sided-long-edge",
      "two-sided-short-edge"
    };

    response = ippNew();

    ippAddRange(response, IPP_TAG_PRINTER, "copies-supported", 1, 1);
    ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_MIMETYPE, "document-format-supported", NULL, "application/vnd.hp-pcl");
    ippAddInteger(response, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "media-bottom-margin-supported", 635);

    media_col_database = ippAddCollections(response, IPP_TAG_PRINTER, "media-col-database", (int)(sizeof(media_col_sizes) / sizeof(media_col_sizes[0])), NULL);
    for (i = 0; i < (sizeof(media_col_sizes) / sizeof(media_col_sizes[0])); i ++)
    {
      media_col = create_media_col(media_supported[i], NULL, NULL, media_col_sizes[i][0], media_col_sizes[i][1], 635);

      ippSetCollection(response, &media_col_database, i, media_col);

      ippDelete(media_col);
    }

    media_col = create_media_col(media_supported[0], NULL, NULL, media_col_sizes[0][0], media_col_sizes[0][1], 635);
    ippAddCollection(response, IPP_TAG_PRINTER, "media-col-default", media_col);
    ippDelete(media_col);

    media_col = create_media_col(media_supported[0], NULL, NULL, media_col_sizes[0][0], media_col_sizes[0][1], 635);
    ippAddCollection(response, IPP_TAG_PRINTER, "media-col-ready", media_col);
    ippDelete(media_col);

    ippAddStrings(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-col-supported", sizeof(media_col_supported) / sizeof(media_col_supported[0]), NULL, media_col_supported);
    ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-default", NULL, media_supported[0]);
    ippAddInteger(response, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "media-left-margin-supported", 635);
    ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-ready", NULL, media_supported[0]);
    ippAddInteger(response, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "media-right-margin-supported", 635);

    media_size_supported = ippAddCollections(response, IPP_TAG_PRINTER, "media-size-supported", (int)(sizeof(media_col_sizes) / sizeof(media_col_sizes[0])), NULL);
    for (i = 0; i < (int)(sizeof(media_col_sizes) / sizeof(media_col_sizes[0])); i ++)
    {
      ipp_t *size = create_media_size(media_col_sizes[i][0], media_col_sizes[i][1]);

      ippSetCollection(response, &media_size_supported, i, size);
      ippDelete(size);
    }

    ippAddStrings(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-supported", (int)(sizeof(media_supported) / sizeof(media_supported[0])), NULL, media_supported);
    ippAddInteger(response, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "media-top-margin-supported", 635);
    ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "print-color-mode-default", NULL, "monochrome");
    ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "print-color-mode-supported", NULL, "monochrome");
    ippAddInteger(response, IPP_TAG_PRINTER, IPP_TAG_ENUM, "print-quality-default", IPP_QUALITY_NORMAL);
    ippAddIntegers(response, IPP_TAG_PRINTER, IPP_TAG_ENUM, "print-quality-supported", (int)(sizeof(quality_supported) / sizeof(quality_supported[0])), quality_supported);
    ippAddResolution(response, IPP_TAG_PRINTER, "printer-resolution-default", IPP_RES_PER_INCH, 300, 300);
        ippAddResolutions(response, IPP_TAG_PRINTER, "printer-resolution-supported", (int)(sizeof(resolution_supported) / sizeof(resolution_supported[0])), IPP_RES_PER_INCH, resolution_supported, resolution_supported);
    ippAddInteger(response, IPP_TAG_PRINTER, IPP_TAG_ENUM, "printer-state", IPP_PSTATE_IDLE);
    ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "printer-state-reasons", NULL, "none");
    ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "sides-default", NULL, "two-sided-long-edge");
        ippAddStrings(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "sides-supported", (int)(sizeof(sides_supported) / sizeof(sides_supported[0])), NULL, sides_supported);
  }

  return (response);
}


/*
 * 'make_uuid()' - Make a RFC 4122 URN UUID from the device URI.
 *
 * NULL device URIs are (appropriately) mapped to "file://hostname/dev/null".
 */

static void
make_uuid(const char *device_uri,	/* I - Device URI or NULL */
          char       *uuid,		/* I - UUID string buffer */
	  size_t     uuidsize)		/* I - Size of UUID buffer */
{
  char			nulluri[1024];	/* NULL URI buffer */
  unsigned char		sha256[32];	/* SHA-256 hash */


 /*
  * Use "file://hostname/dev/null" if the device URI is NULL...
  */

  if (!device_uri)
  {
    char	host[1024];		/* Hostname */


    httpGetHostname(NULL, host, sizeof(host));
    httpAssembleURI(HTTP_URI_CODING_ALL, nulluri, sizeof(nulluri), "file", NULL, host, 0, "/dev/null");
    device_uri = nulluri;
  }

 /*
  * Build a version 3 UUID conforming to RFC 4122 based on the SHA-256 hash of
  * the device URI.
  */

  cupsHashData("sha-256", device_uri, strlen(device_uri), sha256, sizeof(sha256));

  snprintf(uuid, uuidsize, "urn:uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", sha256[16], sha256[17], sha256[18], sha256[19], sha256[20], sha256[21], (sha256[22] & 15) | 0x30, sha256[23], (sha256[24] & 0x3f) | 0x40, sha256[25], sha256[26], sha256[27], sha256[28], sha256[29], sha256[30], sha256[31]);

  if (verbosity)
    plogf(NULL, "UUID for '%s' is '%s'.", device_uri, uuid);
}


/*
 * 'password_cb()' - Password callback.
 */

static const char *			/* O - Password string */
password_cb(const char *prompt,		/* I - Prompt (unused) */
            http_t     *http,		/* I - Connection (unused) */
	    const char *method,		/* I - Method (unused) */
	    const char *resource,	/* I - Resource path (unused) */
	    void       *user_data)	/* I - Password string */
{
  (void)prompt;
  (void)http;
  (void)method;
  (void)resource;

  return ((char *)user_data);
}
#ifdef _WIN32
int gettimeofday(struct timeval* tp, struct timezone* tzp)
{
    // Note: some broken versions only have 8 trailing zero's, the correct epoch has 9 trailing zero's
    // This magic number is the number of 100 nanosecond intervals since January 1, 1601 (UTC)
    // until 00:00:00 January 1, 1970 
    static const uint64_t EPOCH = ((uint64_t)116444736000000000ULL);

    SYSTEMTIME  system_time;
    FILETIME    file_time;
    uint64_t    time;

    GetSystemTime(&system_time);
    SystemTimeToFileTime(&system_time, &file_time);
    time = ((uint64_t)file_time.dwLowDateTime);
    time += ((uint64_t)file_time.dwHighDateTime) << 32;

    tp->tv_sec = (long)((time - EPOCH) / 10000000L);
    tp->tv_usec = (long)(system_time.wMilliseconds * 1000);
    return 0;
}
#endif

/*
 * 'plogf()' - Log a message to stderr.
 */

static void
plogf(proxy_job_t *pjob,			/* I - Proxy job, if any */
      const char  *message,		/* I - Message */
      ...)				/* I - Additional arguments as needed */
{
  char		temp[1024];		/* Temporary message string */
  va_list	ap;			/* Pointer to additional arguments */
  struct timeval curtime;		/* Current time */
  struct tm	curdate;		/* Current date and time */


#ifdef _WIN32
  _cups_gettimeofday(&curtime, NULL);
  time_t tv_sec = (time_t)curtime.tv_sec;
  gmtime_s(&curdate, &tv_sec);
#else
  gettimeofday(&curtime, NULL);
  gmtime_r(&curtime.tv_sec, &curdate);
#endif /* _WIN32 */

  if (pjob)
    snprintf(temp, sizeof(temp), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ  [Job %d] %s\n", curdate.tm_year + 1900, curdate.tm_mon + 1, curdate.tm_mday, curdate.tm_hour, curdate.tm_min, curdate.tm_sec, (int)curtime.tv_usec / 1000, pjob->remote_job_id, message);
  else
    snprintf(temp, sizeof(temp), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ  %s\n", curdate.tm_year + 1900, curdate.tm_mon + 1, curdate.tm_mday, curdate.tm_hour, curdate.tm_min, curdate.tm_sec, (int)curtime.tv_usec / 1000, message);

  va_start(ap, message);
  vfprintf(stderr, temp, ap);
  va_end(ap);
}

/*
 * 'proxy_confUpdate()' - Relay jobs to the local printer.
 */
    static void *				/* O - Thread exit status */
proxy_confUpdate(proxy_info_t *info)		/* I - Printer and device info */
{
    printf("proxy_confUpdate called \n");
    ipp_t* device_attrs;	/* Device attributes */
    unsigned	interval = 1;		// Current interval
    
    cups_dest_t	*dest;			/* Destination for printer URI */
    dest = cupsGetDestWithURI("infra", info->printer_uri);
    while ((info->http = cupsConnectDest(dest, CUPS_DEST_FLAGS_DEVICE, 30000, NULL, info->resource, sizeof(info->resource), NULL, NULL)) == NULL)
    {
    	interval = FIB_NEXT(interval);
        //int interval = 1 + (CUPS_RAND() % 30);
        /* Retry interval */

        plogf(NULL, "'%s' is not responding, retrying in %d seconds.", info->printer_uri, interval);
        sleep(FIB_VALUE(interval));
    }
    while (!info->done)
    {
        device_attrs = get_device_attrs(info->device_uri);
        //info->device_attrs = device_attrs;
        //copy_attributes(oldAttrList,device_attrs);
        if (!update_device_attrs(info->http, info->printer_uri, info->resource, info->device_uuid, NULL, device_attrs)){
            return (NULL);
        }
        /*2 min cycle*/
        sleep(120);
    }
    return (NULL);
}

/*
 * 'proxy_jobs()' - Relay jobs to the local printer.
 */

static void *				/* O - Thread exit status */
proxy_jobs(proxy_info_t *info)		/* I - Printer and device info */
{
  cups_dest_t	*dest;			/* Destination for printer URI */
  proxy_job_t	*pjob;			/* Current job */
//  ipp_t		*new_attrs;		/* New device attributes */
  unsigned	interval = 1;		// Current interval


 /*
  * Connect to the infrastructure printer...
  */

    printf("Job processing thread starting. \n");
  if (verbosity)
    plogf(NULL, "Job processing thread starting.");

  if (password)
    cupsSetPasswordCB(password_cb, password);

  dest = cupsGetDestWithURI("infra", info->printer_uri);
    if (verbosity)
        plogf(NULL, "Connecting to '%s'.", info->printer_uri);

  if (verbosity)
    plogf(NULL, "Connecting to '%s'.", info->printer_uri);

  while ((info->http = cupsConnectDest(dest, CUPS_DEST_FLAGS_DEVICE, 30000, NULL, info->resource, sizeof(info->resource), NULL, NULL)) == NULL)
  {
    interval = FIB_NEXT(interval);

    plogf(NULL, "'%s' is not responding, retrying in %u seconds.", info->printer_uri, FIB_VALUE(interval));
    sleep(FIB_VALUE(interval));
  }

  cupsFreeDests(1, dest);

  if (verbosity)
    plogf(NULL, "Connected to '%s'.", info->printer_uri);

  cupsMutexLock(&jobs_mutex);

  while (!info->done)
  {
   /*
    * Look for a fetchable job...
    */

    if (verbosity)
      plogf(NULL, "Checking for queued jobs.");

    cupsRWLockRead(&jobs_rwlock);
    for (pjob = (proxy_job_t *)cupsArrayGetFirst(jobs_lst); pjob; pjob = (proxy_job_t *)cupsArrayGetNext(jobs_lst))
    {
      if (pjob->local_job_state == IPP_JSTATE_PENDING && pjob->remote_job_state < IPP_JSTATE_CANCELED)
        break;
    }
    cupsRWUnlock(&jobs_rwlock);

    if (pjob)
    {
     /*
      * Process this job...
      */

      run_job(info, pjob);
    }
    else
    {
     /*
      * We didn't have a fetchable job so purge the job cache and wait for more
      * jobs...
      */

      cupsRWLockWrite(&jobs_rwlock);
      for (pjob = (proxy_job_t *)cupsArrayGetFirst(jobs_lst); pjob; pjob = (proxy_job_t *)cupsArrayGetNext(jobs_lst))
      {
	if (pjob->remote_job_state >= IPP_JSTATE_CANCELED)
	  cupsArrayRemove(jobs_lst, pjob);
      }
      cupsRWUnlock(&jobs_rwlock);

      if (verbosity)
        plogf(NULL, "Waiting for jobs.");

      cupsCondWait(&jobs_cond, &jobs_mutex, 15.0);
    }
  }

  cupsMutexUnlock(&jobs_mutex);

  return (NULL);
}

static int send_printer_Details(
        http_t     *http,			/* I - Connection to printer */
        const char *printer_uri,		/* I - Printer URI */
        const char *notifications_uri , /* Universal Print Support, Notifications URI */
        const char *resource,		/* I - Resource path */
        const char *device_uri,		/* I - Device URI, if any */
        const char *device_uuid)		/* I - Device UUID */
{
    ipp_t		*request,		/* IPP request */
                *response;		/* IPP response */
    int		subscription_id = 0;	/* Subscription ID */
    static const char * const UpdateOutputDeviceAttributes[] =	/* Printer Attributes */
    {
        "media-supported", 
        "media-default", 
        "media-ready", 
        "media-col-database", 
        "media-col-database", 
        "media-col-supported" ,
        "media-col-default", 
        "media-col-ready", 
        "media-size", 
        "media-type", 
        "media-source", 
        "media-bottom-margin", 
        "media-top-margin", 
        "media-right-margin", 
        "media-left-margin"
    };

    //ipp_t	*media_col = ippNew(),		/* media-col value */
    //	*media_size = create_media_size(width, length);
    (void)notifications_uri; //notifications_uri not used anywhere , this line works around some compiler warnings.
    (void)device_uri;
    (void)device_uuid;
    /*
     * Create a printer subscription to monitor for events...
     */

    request = ippNewRequest(IPP_OP_UPDATE_OUTPUT_DEVICE_ATTRIBUTES);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, printer_uri);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());
    //ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL, "urn:uuid:7a86562f-334d-4af1-bf5b-ae022e8b45c5"); //Test:cloud-device-id
    //ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL, "urn:uuid:a03231b9-223d-4cfc-bbb5-6c9b8cdc0dae"); //Test:cloud-device-id

    //ippAddCollection(request, IPP_TAG_PRINTER, "media-size", media_size);
    ippAddRange(request, IPP_TAG_PRINTER, "copies-supported", 1, 1);
    ippAddInteger(request, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "media-bottom-margin-supported", 635);
    response = cupsDoRequest(http, request, resource);

    if (cupsGetError() != IPP_STATUS_OK)
    {
        plogf(NULL, "Unable to monitor events on '%s': %s", printer_uri, cupsGetErrorString());
        return (0);
    }

    ippDelete(response);

    return (subscription_id);
}

/*
 * 'register_printer()' - Register the printer (output device) with the Infrastructure Printer.
 */

static int				/* O - Subscription ID */
register_printer(
    http_t     *http,			/* I - Connection to printer */
    const char *printer_uri,		/* I - Printer URI */
    const char *resource,		/* I - Resource path */
    const char *device_uri,		/* I - Device URI, if any */
    const char *device_uuid)		/* I - Device UUID */
{
  ipp_t		*request,		/* IPP request */
		*response;		/* IPP response */
  ipp_attribute_t *attr;		/* Attribute in response */
  int		subscription_id = 0;	/* Subscription ID */
  static const char * const events[] =	/* Events to monitor */
  {
    "document-config-changed",
    "document-state-changed",
    "job-config-changed",
    "job-fetchable",
    "job-state-changed",
    "printer-config-changed",
    "printer-state-changed"
  };


  (void)device_uri;
  (void)device_uuid;
#if 0
    ipp_t	*request1,		/* Get-Printer-Attributes request */
            *response1;		/* IPP response */
    request1 = ippNewRequest(IPP_OP_GET_PRINTER_ATTRIBUTES);
    ippAddString(request1, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, device_uri);
    ippAddString(request1, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());
    ippAddStrings(request1, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", (int)(sizeof(printer_attrs) / sizeof(printer_attrs[0])), NULL, printer_attrs);

    response1 = cupsDoRequest(http, request1, resource);

    if (cupsGetError() >= IPP_STATUS_ERROR_BAD_REQUEST)
    {
        fprintf(stderr, "ippproxy: Device at '%s' returned error: %s\n", device_uri, cupsGetErrorString());
        ippDelete(response1);
        response1 = NULL;
    }
#endif
 /*
  * Create a printer subscription to monitor for events...
  */

    request = ippNewRequest(IPP_OP_CREATE_PRINTER_SUBSCRIPTIONS);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, printer_uri);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL, device_uuid); //Test:cloud-device-id

  ippAddString(request, IPP_TAG_SUBSCRIPTION, IPP_TAG_KEYWORD, "notify-pull-method", NULL, "ippget");
  ippAddStrings(request, IPP_TAG_SUBSCRIPTION, IPP_TAG_KEYWORD, "notify-events", (int)(sizeof(events) / sizeof(events[0])), NULL, events);
  ippAddInteger(request, IPP_TAG_SUBSCRIPTION, IPP_TAG_INTEGER, "notify-lease-duration", 0);

  response = cupsDoRequest(http, request, resource);

  if (cupsGetError() != IPP_STATUS_OK)
  {
    plogf(NULL, "Unable to monitor events on '%s': %s", printer_uri, cupsGetErrorString());
    return (0);
  }

  if ((attr = ippFindAttribute(response, "notify-subscription-id", IPP_TAG_INTEGER)) != NULL)
  {
    subscription_id = ippGetInteger(attr, 0);

    if (verbosity)
      plogf(NULL, "Monitoring events with subscription #%d.", subscription_id);
  }
  else
  {
    plogf(NULL, "Unable to monitor events on '%s': No notify-subscription-id returned.", printer_uri);
  }

  ippDelete(response);

  return (subscription_id);
}


/*
 * 'run_job()' - Fetch and print a job.
 */

static void
run_job(proxy_info_t *info,		/* I - Proxy information */
        proxy_job_t  *pjob)		/* I - Proxy job to fetch and print */
{
  ipp_t		*request,		/* IPP request */
		*job_attrs,		/* Job attributes */
		*doc_attrs;		/* Document attributes */
  int		num_docs,		/* Number of documents */
		doc_number;		/* Current document number */
  ipp_attribute_t *doc_formats;		/* Supported document formats */
  const char	*doc_format = NULL;	/* Document format we want... */


 /*
  * Figure out the output format we want to use...
  */

  doc_formats = ippFindAttribute(info->device_attrs, "document-format-supported", IPP_TAG_MIMETYPE);

  if (info->outformat)
    doc_format = info->outformat;
  else if (!ippContainsString(doc_formats, "application/pdf"))
  {
    if (ippContainsString(doc_formats, "image/urf"))
      doc_format = "image/urf";
    else if (ippContainsString(doc_formats, "image/pwg-raster"))
      doc_format = "image/pwg-raster";
    else if (ippContainsString(doc_formats, "application/vnd.hp-pcl"))
      doc_format = "application/vnd.hp-pcl";
  }

 /*
  * Fetch the job...
  */
    static const char * const jobAttrs[] =/* Printer attributes we are interested in */
    {
            "number-of-documents"
    };

  request = ippNewRequest(IPP_OP_FETCH_JOB);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->printer_uri);
  ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", pjob->remote_job_id);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL, info->device_uuid);
    //ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());
    ippAddStrings(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", (int)(sizeof(jobAttrs) / sizeof(jobAttrs[0])), NULL, jobAttrs);

  if (httpReconnect(info->http, 30000, NULL))
  {
    job_attrs = cupsDoRequest(info->http, request, info->resource);//Error Seen: Unable to update the job state: client-error-not-possible 
    const char		*message;	/* Document format */
    if ((message = ippGetString(ippFindAttribute(job_attrs , "detailed-status-message", IPP_TAG_TEXT), 0, NULL)) == NULL){
    }
    plogf(pjob, "message: %s", message);
  }
  else
  {
    ippDelete(request);
    job_attrs = NULL;
  }

  if (!job_attrs || cupsGetError() >= IPP_STATUS_REDIRECTION_OTHER_SITE)
  {
   /*
    * Cannot proxy this job...
    */

    if (cupsGetError() == IPP_STATUS_ERROR_NOT_FETCHABLE)
    {
      plogf(pjob, "Job already fetched by another printer.");
      pjob->local_job_state = IPP_JSTATE_COMPLETED;
      ippDelete(job_attrs);
      return;
    }

    plogf(pjob, "Unable to fetch job: %s", cupsGetErrorString());
    pjob->local_job_state = IPP_JSTATE_ABORTED;
    goto update_job;
  }

  request = ippNewRequest(IPP_OP_ACKNOWLEDGE_JOB);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->printer_uri);
  ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", pjob->remote_job_id);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL, info->device_uuid);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());

  ippDelete(cupsDoRequest(info->http, request, info->resource));

  if (cupsGetError() >= IPP_STATUS_REDIRECTION_OTHER_SITE)
  {
    plogf(pjob, "Unable to acknowledge job: %s", cupsGetErrorString());
    pjob->local_job_state = IPP_JSTATE_ABORTED;
    return;
  }

  num_docs = ippGetInteger(ippFindAttribute(job_attrs, "number-of-documents", IPP_TAG_INTEGER), 0);
  num_docs=1;

  plogf(pjob, "Fetched job with %d documents.", num_docs);

 /*
  * Then get the document data for each document in the job...
  */

  pjob->local_job_state = IPP_JSTATE_PROCESSING;

  update_job_status(info, pjob);

  for (doc_number = 1; doc_number <= num_docs; doc_number ++)
  {
    if (pjob->remote_job_state >= IPP_JSTATE_ABORTED)
      break;

    update_document_status(info, pjob, doc_number, IPP_DSTATE_PROCESSING);

    request = ippNewRequest(IPP_OP_FETCH_DOCUMENT);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->printer_uri);
    ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", pjob->remote_job_id);
    //ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "document-number", doc_number);
        doc_number = 1;
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL, info->device_uuid);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());
    if (doc_format)
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_MIMETYPE, "document-format-accepted", NULL, doc_format);
//    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "compression-accepted", NULL, "gzip");

    cupsSendRequest(info->http, request, info->resource, ippGetLength(request));
    doc_attrs = cupsGetResponse(info->http, info->resource);
    ippDelete(request);

    if (!doc_attrs || cupsGetError() >= IPP_STATUS_REDIRECTION_OTHER_SITE)
    {
      plogf(pjob, "Unable to fetch document #%d: %s", doc_number, cupsGetErrorString());

      pjob->local_job_state = IPP_JSTATE_ABORTED;
      ippDelete(doc_attrs);
      break;
    }

    if (pjob->remote_job_state < IPP_JSTATE_ABORTED)
    {
     /*
      * Send document to local printer...
      */

            plogf(pjob, "send the document to local printer ");
      send_document(info, pjob, job_attrs, doc_attrs, doc_number);
    }

   /*
    * Acknowledge receipt of the document data...
    */

    ippDelete(doc_attrs);

    request = ippNewRequest(IPP_OP_ACKNOWLEDGE_DOCUMENT);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->printer_uri);
    ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", pjob->remote_job_id);
    ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "document-number", doc_number);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL, info->device_uuid);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());

    ippDelete(cupsDoRequest(info->http, request, info->resource));
  }

  pjob->local_job_state = IPP_JSTATE_COMPLETED;

 /*
  * Update the job state and return...
  */

  update_job:

  ippDelete(job_attrs);

  update_job_status(info, pjob);
}


/*
update attributes
*/
static void update_attributes(
    http_t* http,			/* I - Connection to printer */
    const char* printer_uri,		/* I - Printer URI */
    const char* resource,		/* I - Resource path */
    int        subscription_id,		/* I - Subscription ID */
    const char* device_uri,		/* I - Device URI, if any */
    const char* device_uuid,		/* I - Device UUID */
    const char* outformat) {
    ipp_t* device_attrs;	/* Device attributes */
    //ipp_t* request,	/* IPP request */
    //    * response;	/* IPP response */
    proxy_info_t		info;		/* Information for proxy thread */
    /*
     * Query the printer...
     */

    device_attrs = get_device_attrs(device_uri);

    /*
     * Initialize the local jobs array...
     */

    jobs_lst = cupsArrayNew((cups_array_cb_t)compare_jobs, NULL, NULL, 0, NULL, (cups_afree_cb_t)free);

    memset(&info, 0, sizeof(info));

    info.printer_uri = printer_uri;
    info.printer_uri = printer_uri;
    info.device_uri = device_uri;
    info.device_uuid = device_uuid;
    info.device_attrs = device_attrs;
    info.outformat = outformat;

    jobs_thread = cupsThreadCreate((cups_thread_func_t)proxy_jobs, &info);

    /*
     * Register the output device...
     */

    if (!update_device_attrs(http, printer_uri, resource, device_uuid, NULL, device_attrs))
        return;
}

/*
 * 'run_printer()' - Run the printer until no work remains.
 */

static void
run_printer(
    http_t     *http,			/* I - Connection to printer */
        http_t     *notifications_http,			/* I - Connection to notifications end point printer */
    const char *printer_uri,		/* I - Printer URI */
        const char * notifications_uri,   //notification uri
    const char *resource,		/* I - Resource path */
    int        subscription_id,		/* I - Subscription ID */
    const char *device_uri,		/* I - Device URI, if any */
    const char *device_uuid,		/* I - Device UUID */
    const char *outformat)		/* I - Output format */
{
  ipp_attribute_t	*attr;		/* IPP attribute */
  const char		*name,		/* Attribute name */
			*event;		/* Current event */
  int			job_id;		/* Job ID, if any */
  ipp_jstate_t		job_state;	/* Job state, if any */
  int			seq_number = 1;	/* Current event sequence number */
  int			get_interval;	/* How long to sleep */
  ipp_t* device_attrs,	/* Device attributes */
	  * request,	/* IPP request */
	  * response;	/* IPP response */
  proxy_info_t		info;		/* Information for proxy thread */
    proxy_info_t		confUpdinfo;		/* Information for proxy thread */



 /*
  * Query the printer...
  */

  device_attrs = get_device_attrs(device_uri);

 /*
  * Initialize the local jobs array...
  */

  //jobs = cupsArrayNew((cups_array_cb_t)compare_jobs, NULL, NULL, 0, NULL, (cups_afree_cb_t)free);

  memset(&info, 0, sizeof(info));
    memset(&confUpdinfo, 0, sizeof(confUpdinfo));

#ifdef _WIN32
    strcpy_s(info.resource, strlen(resource) + 1, resource);
    strcpy_s(confUpdinfo.resource, strlen(resource) + 1, resource);
#else 
    strncat(info.resource,resource,strlen(resource) + 1);
    strncat(confUpdinfo.resource,resource,strlen(resource) + 1);
#endif
  info.printer_uri  = printer_uri;
  info.device_uri   = device_uri;
  info.device_uuid  = device_uuid;
  info.device_attrs = device_attrs;
  info.outformat    = outformat;

  confUpdinfo.printer_uri = printer_uri;
  confUpdinfo.device_uri = device_uri;
  confUpdinfo.device_uuid = device_uuid;
  confUpdinfo.device_attrs = device_attrs;
  confUpdinfo.outformat = outformat;

  jobs_thread = cupsThreadCreate((cups_thread_func_t)proxy_jobs, &info);

    if (!update_device_attrs(http, printer_uri, resource, device_uuid, NULL, device_attrs))
        return;
    //pass "updating attrs to azure" onto thread.
    confUpdate_thread = cupsThreadCreate((cups_thread_func_t)proxy_confUpdate, &confUpdinfo);
    printf("proxy_confUpdate started \n");
    plogf(NULL, "create thread: %s", cupsGetErrorString());
    
    jobs_lst = cupsArrayNew((cups_array_cb_t)compare_jobs, NULL, NULL, 0, NULL, (cups_afree_cb_t)free);
    cups_job_t* jobs = NULL;
    httpReconnect(http,3000,NULL);
#if 1
	int res = cupsGetJobs_proxy(http,		/* I - Connection to server or @code CUPS_HTTP_DEFAULT@ */
        &jobs,
        resource,
        printer_uri,
        device_uuid);
#endif	
 /*
  * Register the output device...
  */
/*
  if (!update_device_attrs(http, printer_uri, resource, device_uuid, NULL, device_attrs))
    return;

  if (!update_remote_jobs(http, printer_uri, resource, device_uuid))
    return;
*/
  while (!stop_running)
  {
   /*
    * See if we have any work to do...
    */
        ipp_op_t ippOperationRequested = IPP_OP_GET_NOTIFICATIONS;
        request = ippNewRequest(ippOperationRequested);

        ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, notifications_uri);
    ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "notify-subscription-ids", subscription_id);
    ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "notify-sequence-numbers", seq_number);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL, device_uuid);
    //ippAddBoolean(request, IPP_TAG_OPERATION, "notify-wait", 1);

    if (verbosity)
      plogf(NULL, "Sending Get-Notifications request.");

        response = cupsDoRequest(notifications_http, request, resource);

    if (verbosity)
      plogf(NULL, "Get-Notifications response: %s", ippErrorString(cupsGetError()));
    print_conn_info(response,"run_printer");

    if ((attr = ippFindAttribute(response, "notify-get-interval", IPP_TAG_INTEGER)) != NULL)
      get_interval = ippGetInteger(attr, 0);
    else
      get_interval = 30;

    if (verbosity)
      plogf(NULL, "notify-get-interval=%d", get_interval);

    for (attr = ippGetFirstAttribute(response); attr; attr = ippGetNextAttribute(response))
    {
      if (ippGetGroupTag(attr) != IPP_TAG_EVENT_NOTIFICATION || !ippGetName(attr))
        continue;

      event     = NULL;
      job_id    = 0;
      job_state = IPP_JSTATE_PENDING;

      while (ippGetGroupTag(attr) == IPP_TAG_EVENT_NOTIFICATION && (name = ippGetName(attr)) != NULL)
      {
          if (!strcmp(name, "notify-subscribed-event") && ippGetValueTag(attr) == IPP_TAG_KEYWORD){
              event = ippGetString(attr, 0, NULL);
              plogf(NULL, "notify-subscribed-event=%s", event);
          }
          else if (!strcmp(name, "job-id") && ippGetValueTag(attr) == IPP_TAG_INTEGER){
              job_id = ippGetInteger(attr, 0);
              plogf(NULL, "notify-job-id/job-id=%d", job_id);
          }
          else if (!strcmp(name, "job-state") && ippGetValueTag(attr) == IPP_TAG_ENUM){
              job_state = (ipp_jstate_t)ippGetInteger(attr, 0);
              plogf(NULL, "job-state=%d", (int)job_state);
          }
          else if (!strcmp(name, "notify-sequence-number") && ippGetValueTag(attr) == IPP_TAG_INTEGER)
          {
              int new_seq = ippGetInteger(attr, 0);

              if (new_seq >= seq_number)
                  seq_number = new_seq + 1;
                    plogf(NULL, "notify-sequence-number=%d", new_seq);

          }
          else if (!strcmp(name, "printer-state-reasons") && ippContainsString(attr, "identify-printer-requested")){
                    acknowledge_identify_printer(http, printer_uri,notifications_uri ,  resource, device_uuid);
                }

        attr = ippGetNextAttribute(response);
      }

      if (event && job_id)
      {
				//reset count max 720 #6hrs
				reset_connection_count = 0;
        if (!strcmp(event, "job-fetchable") && job_id)
	{
	 /*
	  * Queue up new job...
	  */

          proxy_job_t *pjob = find_job(job_id);

	  if (!pjob)
	  {
	   /*
	    * Not already queued up, make a new one...
	    */

            if ((pjob = (proxy_job_t *)calloc(1, sizeof(proxy_job_t))) != NULL)
            {
             /*
              * Add job and then let the proxy thread know we added something...
              */

              pjob->remote_job_id    = job_id;
              pjob->remote_job_state = (int)job_state;
              pjob->local_job_state  = IPP_JSTATE_PENDING;

	      plogf(pjob, "Job is now fetchable, queuing up.", pjob);

              cupsRWLockWrite(&jobs_rwlock);
              cupsArrayAdd(jobs_lst, pjob);
              cupsRWUnlock(&jobs_rwlock);

	      cupsCondBroadcast(&jobs_cond);
	    }
	    else
	    {
	      plogf(NULL, "Unable to add job %d to jobs queue.", job_id);
	    }
          }
	}
	else if (!strcmp(event, "job-state-changed") && job_id)
	{
	 /*
	  * Update our cached job info...  If the job is currently being
	  * proxied and the job has been canceled or aborted, the code will see
	  * that and stop printing locally.
	  */

	  proxy_job_t *pjob = find_job(job_id);

          if (pjob)
          {
	    pjob->remote_job_state = (int)job_state;

	    plogf(pjob, "Updated remote job-state to '%s'.", ippEnumString("job-state", (int)job_state));

	    cupsCondBroadcast(&jobs_cond);
	  }
	}
      }
    }

   /*
    * Pause before our next poll of the Infrastructure Printer...
    */

    if (get_interval > 0 && get_interval < 3600)
      sleep((unsigned)get_interval);
    else
      sleep(30);
        /*
        plogf(NULL, "updating the device attrs again \n");
        device_attrs = get_device_attrs(device_uri);
        if (!update_device_attrs(http, printer_uri, resource, device_uuid, NULL, device_attrs))
            return;
	    */

    reset_connection_count += 1;
    httpReconnect(http, 3000, NULL);
    httpReconnect(notifications_http,3000,NULL);
    if(reset_connection_count > MAX_RESET_WAIT_COUNT)
        stop_running = 1;
  }

 /*
  * Stop the job proxy thread...
  */

  cupsCondBroadcast(&jobs_cond);
  cupsThreadCancel(jobs_thread);
  cupsThreadWait(jobs_thread);
}


/*
 * 'send_document()' - Send a proxied document to the local printer.
 */

static void
send_document(proxy_info_t *info,	/* I - Proxy information */
              proxy_job_t  *pjob,	/* I - Proxy job */
              ipp_t        *job_attrs,	/* I - Job attributes */
              ipp_t        *doc_attrs,	/* I - Document attributes */
              int          doc_number)	/* I - Document number */
{
  char		scheme[32],		/* URI scheme */
		userpass[256],		/* URI user:pass */
		host[256],		/* URI host */
		resource[256],		/* URI resource */
		service[32];		/* Service port */
  int		port;			/* URI port number */
  http_addrlist_t *list;		/* Address list for socket */
  const char	*doc_compression;	/* Document compression, if any */
  size_t	doc_total = 0;		/* Total bytes read */
  ssize_t	doc_bytes;		/* Bytes read/written */
  char		doc_buffer[16384];	/* Copy buffer */


  if ((doc_compression = ippGetString(ippFindAttribute(doc_attrs, "compression", IPP_TAG_KEYWORD), 0, NULL)) != NULL && !strcmp(doc_compression, "none"))
    doc_compression = NULL;

  if (httpSeparateURI(HTTP_URI_CODING_ALL, info->device_uri, scheme, sizeof(scheme), userpass, sizeof(userpass), host, sizeof(host), &port, resource, sizeof(resource)) < HTTP_URI_STATUS_OK)
  {
    plogf(pjob, "Invalid device URI '%s'.", info->device_uri);
    pjob->local_job_state = IPP_JSTATE_ABORTED;
    return;
  }

  snprintf(service, sizeof(service), "%d", port);
  if ((list = httpAddrGetList(host, AF_UNSPEC, service)) == NULL)
  {
    plogf(pjob, "Unable to lookup device URI host '%s': %s", host, cupsGetErrorString());
    pjob->local_job_state = IPP_JSTATE_ABORTED;
    return;
  }

  if (!strcmp(scheme, "socket"))
  {
   /*
    * AppSocket connection...
    */

    int		sock;			/* Output socket */

    if (verbosity)
      plogf(pjob, "Connecting to '%s'.", info->device_uri);

    if (!httpAddrConnect(list, &sock, 30000, NULL))
    {
      plogf(pjob, "Unable to connect to '%s': %s", info->device_uri, cupsGetErrorString());
      pjob->local_job_state = IPP_JSTATE_ABORTED;
      httpAddrFreeList(list);
      return;
    }

    if (verbosity)
      plogf(pjob, "Connected to '%s'.", info->device_uri);

    if (doc_compression)
      httpSetField(info->http, HTTP_FIELD_CONTENT_ENCODING, doc_compression);

    while ((doc_bytes = cupsReadResponseData(info->http, doc_buffer, sizeof(doc_buffer))) > 0)
    {
      char	*doc_ptr = doc_buffer,	/* Pointer into buffer */
		*doc_end = doc_buffer + doc_bytes;
					/* End of buffer */

      doc_total += (size_t)doc_bytes;

      while (doc_ptr < doc_end)
      {
        if ((doc_bytes = write(sock, doc_ptr, (size_t)(doc_end - doc_ptr))) > 0)
          doc_ptr += doc_bytes;
      }
    }

    close(sock);

    plogf(pjob, "Local job created, %ld bytes.", (long)doc_total);
  }
  else
  {
    int			i;		/* Looping var */
    http_t		*http;		/* Output HTTP connection */
    http_encryption_t	encryption;	/* Encryption mode */
    ipp_t		*request,	/* IPP request */
			*response;	/* IPP response */
    ipp_attribute_t	*attr;		/* Current attribute */
    int			create_job = 0;	/* Support for Create-Job/Send-Document? */
    const char		*doc_format;	/* Document format */
    ipp_jstate_t	job_state;	/* Current job-state value */
    static const char * const pattrs[] =/* Printer attributes we are interested in */
    {
      "compression-supported",
      "operations-supported"
    };
    static const char * const operation[] =
    {					/* Operation attributes to copy */
      "job-name",
      "job-password",
      "job-password-encryption",
      "job-priority"
    };
    static const char * const job_template[] =
    {					/* Job Template attributes to copy */
      "copies",
      "finishings",
      "finishings-col",
      "job-account-id",
      "job-accounting-user-id",
      "media",
      "media-col",
      "multiple-document-handling",
      "orientation-requested",
      "page-ranges",
      "print-color-mode",
      "print-quality",
            "printer-resolution",
            "output-bin",
            "output-mode",
            "ipp-attribute-fidelity",
            "media-source", 
            "number-up",
            "print-scaling",
            "job-priority",
            "job-hold-until",
            "job-sheets",
      "sides"
    };

    if ((doc_format = ippGetString(ippFindAttribute(doc_attrs, "document-format", IPP_TAG_MIMETYPE), 0, NULL)) == NULL)
      doc_format = "application/octet-stream";

	//Job-originating-user-name is not yet processed by IPP Service. 
	//Hence request job-originating-user-name and send it in requesting-user-name. After Copy remove the job-originating-user-name from job_attrs.
	//TODO support for job-originating-user-name should be provided.
	const char* userNameFromRequest;   /* Document compression, if any */
	userNameFromRequest = ippGetString(ippFindAttribute(job_attrs, "job-originating-user-name", IPP_TAG_NAME), 0, NULL);
	//ipp_attribute_t	*attr_job_originating_user_name;		/* Current attribute */
	//if(attr_job_originating_user_name = ippFindAttribute(job_attrs, "job-originating-user-name", IPP_TAG_NAME) == NULL){//Found the attr. remove it.
	//	ippDeleteAttribute(job_attrs , attr_job_originating_user_name);
	//}
   /*
    * Connect to the IPP/IPPS printer...
    */

    if (port == 443 || !strcmp(scheme, "ipps"))
      encryption = HTTP_ENCRYPTION_ALWAYS;
    else
      encryption = HTTP_ENCRYPTION_IF_REQUESTED;

    if (verbosity)
      plogf(pjob, "Connecting to '%s'.", info->device_uri);

    if ((http = httpConnect(host, port, list, AF_UNSPEC, encryption, 1, 30000, NULL)) == NULL)
    {
      plogf(pjob, "Unable to connect to '%s': %s\n", info->device_uri, cupsGetErrorString());
      httpAddrFreeList(list);
      pjob->local_job_state = IPP_JSTATE_ABORTED;
      return;
    }

    if (verbosity)
      plogf(pjob, "Connected to '%s'.", info->device_uri);

   /*
    * See if it supports Create-Job + Send-Document...
    */

    request = ippNewRequest(IPP_OP_GET_PRINTER_ATTRIBUTES);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->device_uri);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, userNameFromRequest);
    ippAddStrings(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", (int)(sizeof(pattrs) / sizeof(pattrs[0])), NULL, pattrs);

    response = cupsDoRequest(http, request, resource);

    if ((attr = ippFindAttribute(response, "operations-supported", IPP_TAG_ENUM)) == NULL)
    {
      plogf(pjob, "Unable to get list of supported operations from printer.");
      pjob->local_job_state = IPP_JSTATE_ABORTED;
      httpAddrFreeList(list);
      ippDelete(response);
      httpClose(http);
      return;
    }

    create_job = ippContainsInteger(attr, IPP_OP_CREATE_JOB) && ippContainsInteger(attr, IPP_OP_SEND_DOCUMENT);

    if (doc_compression && !ippContainsString(ippFindAttribute(response, "compression-supported", IPP_TAG_KEYWORD), doc_compression))
    {
     /*
      * Decompress raster data to send to printer without compression...
      */

      httpSetField(info->http, HTTP_FIELD_CONTENT_ENCODING, doc_compression);
      doc_compression = NULL;
    }

    ippDelete(response);

   /*
    * Create the job and start printing...
    */

    request = ippNewRequest(create_job ? IPP_OP_CREATE_JOB : IPP_OP_PRINT_JOB);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->device_uri);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, userNameFromRequest);
    if (!create_job)
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_MIMETYPE, "document-format", NULL, doc_format);
    if (!create_job && doc_compression)
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "compression", NULL, doc_compression);
    for (i = 0; i < (int)(sizeof(operation) / sizeof(operation[0])); i ++)
    {
      if ((attr = ippFindAttribute(job_attrs, operation[i], IPP_TAG_ZERO)) != NULL)
      {
	attr = ippCopyAttribute(request, attr, 0);
	ippSetGroupTag(request, &attr, IPP_TAG_OPERATION);
      }
    }

    for (i = 0; i < (int)(sizeof(job_template) / sizeof(job_template[0])); i ++)
    {
      if ((attr = ippFindAttribute(job_attrs, job_template[i], IPP_TAG_ZERO)) != NULL)
	ippCopyAttribute(request, attr, 0);
    }

    if (verbosity)
    {
      plogf(pjob, "%s", ippOpString(ippGetOperation(request)));

      for (attr = ippGetFirstAttribute(request); attr; attr = ippGetNextAttribute(request))
      {
        const char *name = ippGetName(attr);	/* Attribute name */

        if (!name)
        {
          plogf(pjob, "----");
          continue;
	}

        ippAttributeString(attr, doc_buffer, sizeof(doc_buffer));

        plogf(pjob, "%s %s '%s'", name, ippTagString(ippGetValueTag(attr)), doc_buffer);
      }
    }

    if (create_job)
    {
      response = cupsDoRequest(http, request, resource);
      pjob->local_job_id = ippGetInteger(ippFindAttribute(response, "job-id", IPP_TAG_INTEGER), 0);
      ippDelete(response);

      if (pjob->local_job_id <= 0)
      {
	plogf(pjob, "Unable to create local job: %s", cupsGetErrorString());
	pjob->local_job_state = IPP_JSTATE_ABORTED;
	httpAddrFreeList(list);
	httpClose(http);
	return;
      }

      request = ippNewRequest(IPP_OP_SEND_DOCUMENT);
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->device_uri);
      ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", pjob->local_job_id);
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, userNameFromRequest);
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_MIMETYPE, "document-format", NULL, doc_format);
      if (doc_compression)
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "compression", NULL, doc_compression);
      ippAddBoolean(request, IPP_TAG_OPERATION, "last-document", 1);

      if (verbosity)
      {
	plogf(pjob, "%s", ippOpString(ippGetOperation(request)));

	for (attr = ippGetFirstAttribute(request); attr; attr = ippGetNextAttribute(request))
	{
	  const char *name = ippGetName(attr);	/* Attribute name */

	  if (!name)
	  {
	    plogf(pjob, "----");
	    continue;
	  }

	  ippAttributeString(attr, doc_buffer, sizeof(doc_buffer));

	  plogf(pjob, "%s %s '%s'", name, ippTagString(ippGetValueTag(attr)), doc_buffer);
	}
      }
    }

    if (cupsSendRequest(http, request, resource, 0) == HTTP_STATUS_CONTINUE)
    {
      while ((doc_bytes = cupsReadResponseData(info->http, doc_buffer, sizeof(doc_buffer))) > 0)
      {
	doc_total += (size_t)doc_bytes;

        if (cupsWriteRequestData(http, doc_buffer, (size_t)doc_bytes) != HTTP_STATUS_CONTINUE)
          break;
      }
    }

    response = cupsGetResponse(http, resource);

    if (!pjob->local_job_id)
      pjob->local_job_id = ippGetInteger(ippFindAttribute(response, "job-id", IPP_TAG_INTEGER), 0);

    job_state = (ipp_jstate_t)ippGetInteger(ippFindAttribute(response, "job-state", IPP_TAG_ENUM), 0);

    ippDelete(response);

    if (cupsGetError() >= IPP_STATUS_REDIRECTION_OTHER_SITE)
    {
      plogf(pjob, "Unable to create local job: %s", cupsGetErrorString());
      pjob->local_job_state = IPP_JSTATE_ABORTED;
      httpAddrFreeList(list);
      httpClose(http);
      return;
    }

    plogf(pjob, "Local job %d created, %ld bytes.", pjob->local_job_id, (long)doc_total);

    while (pjob->remote_job_state < IPP_JSTATE_CANCELED && job_state < IPP_JSTATE_CANCELED)
    {
      request = ippNewRequest(IPP_OP_GET_JOB_ATTRIBUTES);
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->device_uri);
      ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", pjob->local_job_id);
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", NULL, "job-state");

      response = cupsDoRequest(http, request, resource);

      if (cupsGetError() >= IPP_STATUS_REDIRECTION_OTHER_SITE)
	job_state = IPP_JSTATE_COMPLETED;
      else
        job_state = (ipp_jstate_t)ippGetInteger(ippFindAttribute(response, "job-state", IPP_TAG_ENUM), 0);
            pjob->local_job_state  = job_state;
      ippDelete(response);
    }
        plogf(pjob, "final job_state to be set :%d ", (int)job_state);
        pjob->local_job_state  = job_state;

    if (pjob->remote_job_state == IPP_JSTATE_CANCELED)
    {
     /*
      * Cancel locally...
      */

      plogf(pjob, "Canceling job locally.");

      request = ippNewRequest(IPP_OP_CANCEL_JOB);
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->device_uri);
      ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", pjob->local_job_id);
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());

      ippDelete(cupsDoRequest(http, request, resource));

      if (cupsGetError() >= IPP_STATUS_REDIRECTION_OTHER_SITE)
	plogf(pjob, "Unable to cancel local job: %s", cupsGetErrorString());

      pjob->local_job_state = IPP_JSTATE_CANCELED;
    }

    httpClose(http);
  }

  httpAddrFreeList(list);

  update_document_status(info, pjob, doc_number, IPP_DSTATE_COMPLETED);
}


/*
 * 'sighandler()' - Handle termination signals so we can clean up...
 */

static void
sighandler(int sig)			/* I - Signal */
{
  (void)sig;

  stop_running = 1;
}

static void updateFieryResponse(ipp_t *request){
    static const char * const doc_format_details_supported[] =     
    {
        //"",
        //"",
        "document-source-application-name"
    };
    static const char * const versions[] =     /* Events to monitor */
    {
        "adobe-1.4",
        "adobe-1.5",
        "adobe-1.6"
    };
    static const char * const nlangSupported[] =
    {
        "en-us"
    };
    static const char * const psOptions[] = 
    {
        "auto"
    };
    //EXPECTED: pdf-versions-supported
    ippAddStrings(request, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "pdf-versions-supported", (int)(sizeof(versions) / sizeof(versions[0])), NULL, versions);
    //EXPECTED: print-scaling-default
    ippAddString(request, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "print-scaling-default", NULL, psOptions[0]);
    //EXPECTED: print-scaling-supported
    ippAddStrings(request, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "print-scaling-supported", (int)(sizeof(psOptions) / sizeof(psOptions[0])), NULL, psOptions);
    ippAddString(request, IPP_TAG_PRINTER, IPP_TAG_LANGUAGE, "generated-natural-language-supported", NULL,"en-us");
    ippAddString(request, IPP_TAG_PRINTER, IPP_TAG_LANGUAGE, "natural-language-configured", NULL,"en-us");
    ippAddString(request, IPP_TAG_PRINTER, IPP_TAG_CHARSET, "charset-configured", NULL,"utf-8");
    static const char * const docTypeSupp[] =
    {
        "application/pdf"
    };
    ippAddStrings(request, IPP_TAG_PRINTER, IPP_TAG_MIMETYPE, "document-format-supported", (int)(sizeof(docTypeSupp) / sizeof(docTypeSupp[0])), NULL, docTypeSupp);
    ippAddString(request, IPP_TAG_PRINTER, IPP_TAG_MIMETYPE, "document-format-default", NULL,docTypeSupp[0]);

        static const int media_col_sizes[][2] =
        {
            { 29700, 42000 }, //iso_a3_297x420mm
            { 27940, 43180 }, //na_ledger_11x17in
            { 10477, 24130 }, //na_number-10_4.125x9.5in
            { 11010, 22010 }, //custom_11010x22010mm
            { 22860, 27940 }, //na_9x11_9x11in
            { 27940, 43180 }, //na_ledger_11x17in
            { 21590, 27940 }, //na_letter_8.5x11in
            { 21590, 27940 }, //na_letter_8.5x11in
            { 11990, 23500 }, //custom_11990x23500mm
            { 21590, 27940 }, //na_letter_8.5x11in
            { 21590, 27940 }, //na_letter_8.5x11in
            { 27940, 43180 }, //na_ledger_11x17in
            { 21590, 27940 }, //na_letter_8.5x11in
            { 21590, 27940 }, //na_letter_8.5x11in
            { 21590, 35560 }, //na_legal_8.5x14in
            { 30480, 45720 }, //na_arch-b_12x18in
            { 21000, 29700 }, //iso_a4_210x297mm
            { 21590, 27940 } //na_letter_8.5x11in
        };

        ipp_attribute_t	*media_col_database, *media_size_supported;
        media_size_supported = ippAddCollections(request, IPP_TAG_PRINTER, "media-size-supported", (int)(sizeof(media_col_sizes) / sizeof(media_col_sizes[0])), NULL);
        for (int i = 0; i < (int)(sizeof(media_col_sizes) / sizeof(media_col_sizes[0])); i ++)
        {
            ipp_t *size = create_media_size(media_col_sizes[i][0], media_col_sizes[i][1]);

            ippSetCollection(request, &media_size_supported, i, size);
            ippDelete(size);
        }

        ippAddString(request, IPP_TAG_PRINTER, IPP_TAG_MIMETYPE, "document-format-preferred", NULL, "application/pdf");
        ippAddStrings(request, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "document-format-details-supported", (int)(sizeof(doc_format_details_supported) / sizeof(doc_format_details_supported[0])), NULL, doc_format_details_supported);
        ippAddBoolean(request, IPP_TAG_PRINTER, "job-pages-per-set-supported", 0);
        //document-format-preferred,document-format-details-supported,job-pages-per-set-supported
                        
        
        ipp_t	*document_format_details= ippNew();		/* media-size value */
        ippAddString(document_format_details, IPP_TAG_PRINTER, IPP_TAG_TEXT, "document-source-application-name", NULL, "IPPProxy");
        ippAddCollection(request,IPP_TAG_PRINTER, "document-format-details", document_format_details);

        static const char * const mulitiple_document_handling_keys[] =     
        {
            "single-document",
            "separate-documents-collated-copies"
        };
        ippAddStrings(request, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "multiple-document-handling-supported", (int)(sizeof(mulitiple_document_handling_keys) / sizeof(mulitiple_document_handling_keys[0])), NULL, mulitiple_document_handling_keys);


}
/*
 * 'update_device_attrs()' - Update device attributes on the server.
 */

static int				/* O - 1 on success, 0 on failure */
update_device_attrs(
    http_t     *http,			/* I - Connection to server */
    const char *printer_uri,		/* I - Printer URI */
    const char *resource,		/* I - Resource path */
    const char *device_uuid,		/* I - Device UUID */
    ipp_t      *old_attrs,		/* I - Old attributes */
    ipp_t      *new_attrs)		/* I - New attributes */
{
  int			i,		/* Looping var */
			result;		/* Result of comparison */
    int             retValue;
  ipp_t			*request;	/* IPP request */
  ipp_attribute_t	*attr;		/* New attribute */
  const char		*name;		/* New attribute name */


 /*
  * Update the configuration of the output device...
  */
    printf("update_device_attrs for { %s, %s, %s }\n",printer_uri,resource,device_uuid);

  request = ippNewRequest(IPP_OP_UPDATE_OUTPUT_DEVICE_ATTRIBUTES);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, printer_uri);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL, device_uuid);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());

  for (attr = ippGetFirstAttribute(new_attrs); attr; attr = ippGetNextAttribute(new_attrs))
  {
   /*
    * Add any attributes that have changed...
    */

    if (ippGetGroupTag(attr) != IPP_TAG_PRINTER || (name = ippGetName(attr)) == NULL)
      continue;
        name = ippGetName(attr);
        /*
        if( (0==strncmp(name, "pdf-versions-supported",22))
                || (0==strncmp(name, "print-scaling-default",21))
                || (0==strncmp(name, "print-scaling-supported",23))
                || (0==strncmp(name, "generated-natural-language-supported",36 ))
                || (0==strncmp(name, "natural-language-configured",27 ))
                || (0==strncmp(name, "charset-configured",18 ))
                || (0==strncmp(name, "document-format-supported",25 ))
                || (0==strncmp(name, "media-size-supported",20 ))
                || (0==strncmp(name, "document-format-default",23 )))
        {
            printf("Key=%s skipped \n", name);
            continue;
        }*/

    for (i = 0, result = 1; i < (int)(sizeof(printer_attrs) / sizeof(printer_attrs[0])); i ++)
    {
      if ((result = strcmp(name, printer_attrs[i])) == 0)
      {
       /*
        * This is an attribute we care about...
	*/

        if (!attrs_are_equal(ippFindAttribute(old_attrs, name, ippGetValueTag(attr)), attr)){
                    if( (0==strncmp(name, "media-col-database",18))){ 
                        ipp_t	*col_media_source_prop= ippNew();		/* media-size value */
                        //Find if LEF/SEF
                        //If LEF send long-edge-first and PORTRAIT
                        //Else send short-edge-first and Landscape
                        //ippAddString(col_media_source_prop, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-source-feed-direction",NULL,  "long-edge-first");
                        //ippAddInteger(col_media_source_prop, IPP_TAG_PRINTER, IPP_TAG_ENUM, "media-source-feed-orientation", (int)IPP_ORIENT_PORTRAIT);
                        //int i , count ;
                        //for (i = 0, count = ippGetCount(attr); i < count; i ++){
                        //    ipp_t * attr_value = ippGetCollection(attr,i);
                        //    ippAddCollection(attr_value,IPP_TAG_PRINTER, "media-source-properties", col_media_source_prop);
                        //}

                    }
                    //printf("Updating Key=%s,Value=%d\n", name, ippGetInteger(attr,0));
	  ippCopyAttribute(request, attr, 1);
      }
                break;
    }
  }
    }
    //updateFieryResponse(request);
#if 1 //Move this one too to ipp service
    ipp_t	*document_format_details= ippNew();		/* media-size value */
    ippAddString(document_format_details, IPP_TAG_PRINTER, IPP_TAG_TEXT, "document-source-application-name", NULL, "IPPProxy");
    ippAddCollection(request,IPP_TAG_PRINTER, "document-format-details", document_format_details); 
#endif

  if (httpReconnect(http, 30000, NULL))
  {
    ipp_t		*response;	/* IPP request */
    response = cupsDoRequest(http, request, resource);

    if (cupsGetError() != IPP_STATUS_OK)
    {
      plogf(NULL, "Unable to update the output device with '%s': %s", printer_uri, cupsGetErrorString());
        const char* message;	/* Document format */
        if ((message = ippGetString(ippFindAttribute(response, "detailed-status-message", IPP_TAG_TEXT), 0, NULL)) == NULL) {
            printf("Error : display \n");
            plogf(NULL, "Unable to update the output device with '%s': %s", printer_uri, cupsGetErrorString());
        }
      return (0);
    }
    ippDelete(response);
  }
  else
  {
    ippDelete(request);

    plogf(NULL, "Unable to update the output device with '%s': %s", printer_uri, cupsGetErrorString());
    return (0);
  }

  return (1);
}


/*
 * 'update_document_status()' - Update the document status.
 */

static void
update_document_status(
    proxy_info_t *info,			/* I - Proxy info */
    proxy_job_t  *pjob,			/* I - Proxy job */
    int          doc_number,		/* I - Document number */
    ipp_dstate_t doc_state)		/* I - New document-state value */
{
  ipp_t	*request;			/* IPP request */


  request = ippNewRequest(IPP_OP_UPDATE_DOCUMENT_STATUS);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->printer_uri);
  ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", pjob->remote_job_id);
  ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "document-number", doc_number);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL, info->device_uuid);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());

    plogf(pjob, "updating document state to #%d ", (int)doc_state);
  ippAddInteger(request, IPP_TAG_JOB, IPP_TAG_ENUM, "output-device-document-state", (int)doc_state);
    ippAddInteger(request, IPP_TAG_DOCUMENT, IPP_TAG_INTEGER, "document-number", 1);

  ippDelete(cupsDoRequest(info->http, request, info->resource));

  if (cupsGetError() >= IPP_STATUS_REDIRECTION_OTHER_SITE)
    plogf(pjob, "Unable to update the state for document #%d: %s", doc_number, cupsGetErrorString());
}


/*
 * 'update_job_status()' - Update the job status.
 */

static void
update_job_status(proxy_info_t *info,	/* I - Proxy info */
                  proxy_job_t  *pjob)	/* I - Proxy job */
{
  ipp_t	*request;			/* IPP request */


  request = ippNewRequest(IPP_OP_UPDATE_JOB_STATUS);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->printer_uri);
  ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", pjob->remote_job_id);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL, info->device_uuid);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());

  ippAddInteger(request, IPP_TAG_JOB, IPP_TAG_ENUM, "output-device-job-state", (int)pjob->local_job_state);

  ippDelete(cupsDoRequest(info->http, request, info->resource));

  if (cupsGetError() >= IPP_STATUS_REDIRECTION_OTHER_SITE)
    plogf(pjob, "Unable to update the job state: %s", cupsGetErrorString());
}


//
// 'update_remote_jobs()' - Get the current list of remote, fetchable jobs.
//

static bool				// O - `true` on success, `false` on failure
update_remote_jobs(
    http_t     *http,			// I - Connection to infrastructure printer
    const char *printer_uri,		// I - Printer URI
    const char *resource,		// I - Printer resource path
    const char *device_uuid)		// I - Device UUID
{
  ipp_t			*request,	// IPP request
			*response;	// IPP response
  ipp_attribute_t	*attr;		// IPP attribute
  const char		*name;		// Attribute name
  int			job_id;		// Job ID, if any
  ipp_jstate_t		job_state;	// Job state, if any


  // Get the list of fetchable jobs...
  plogf(NULL, "Getting fetchable jobs...");
  request = ippNewRequest(IPP_OP_GET_JOBS);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, printer_uri);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());
  ippAddString(request, IPP_TAG_OPERATION, IPP_CONST_TAG(IPP_TAG_KEYWORD), "which-jobs", NULL, "fetchable");
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL, device_uuid);

  if ((response = cupsDoRequest(http, request, resource)) == NULL)
  {
    plogf(NULL, "Get-Jobs failed: %s", cupsGetErrorString());
    return (false);
  }

  // Scan the list...
  for (attr = ippGetFirstAttribute(response); attr; attr = ippGetNextAttribute(response))
  {
    // Skip to the start of the next job group...
    while (attr && ippGetGroupTag(attr) != IPP_TAG_JOB)
      attr = ippGetNextAttribute(response);
    if (!attr)
      break;

    // Get the job-id and state...
    job_id    = 0;
    job_state = IPP_JSTATE_PENDING;

    while (attr && ippGetGroupTag(attr) == IPP_TAG_JOB)
    {
      name = ippGetName(attr);
      if (!strcmp(name, "job-id"))
        job_id = ippGetInteger(attr, 0);
      else if (!strcmp(name, "job-state"))
        job_state = (ipp_jstate_t)ippGetInteger(attr, 0);

      attr = ippGetNextAttribute(response);
    }

    if (job_id && (job_state == IPP_JSTATE_PENDING || job_state == IPP_JSTATE_STOPPED))
    {
      // Add this job...
      proxy_job_t *pjob = find_job(job_id);

      if (!pjob)
      {
        // Not already queued up, make a new one...
	if ((pjob = (proxy_job_t *)calloc(1, sizeof(proxy_job_t))) != NULL)
	{
	  // Add job and then let the proxy thread know we added something...
	  pjob->remote_job_id    = job_id;
	  pjob->remote_job_state = (int)job_state;
	  pjob->local_job_state  = IPP_JSTATE_PENDING;

	  plogf(pjob, "Job is now fetchable, queuing up.", pjob);

	  cupsRWLockWrite(&jobs_rwlock);
	  cupsArrayAdd(jobs_lst, pjob);
	  cupsRWUnlock(&jobs_rwlock);

	  cupsCondBroadcast(&jobs_cond);
	}
	else
	{
	  plogf(NULL, "Unable to add job %d to jobs queue.", job_id);
	}
      }
    }
  }

  ippDelete(response);

  return (true);
}

static void*	create_printer_subscriptions(proxy_info_t *info, 
        proxy_job_t *pjob)
{
    ipp_t	*request,			/* IPP request */
            *response;
    static const char * const events[] =	/* Events to monitor */
    {
        "document-config-changed",
        "document-state-changed",
        "job-config-changed",
        "job-fetchable",
        "job-state-changed",
        "printer-config-changed",
        "printer-state-changed"
    };


    request = ippNewRequest(IPP_OP_CREATE_PRINTER_SUBSCRIPTIONS);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->printer_uri);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL, "urn:uuid:a03231b9-223d-4cfc-bbb5-6c9b8cdc0dae"); //Test:cloud-device-id

    ippAddString(request, IPP_TAG_SUBSCRIPTION, IPP_TAG_KEYWORD, "notify-pull-method", NULL, "ippget");
    ippAddStrings(request, IPP_TAG_SUBSCRIPTION, IPP_TAG_KEYWORD, "notify-events", (int)(sizeof(events) / sizeof(events[1])), NULL, events);
    ippAddInteger(request, IPP_TAG_SUBSCRIPTION, IPP_TAG_INTEGER, "notify-lease-duration", 0);

    response = cupsDoRequest(info->http, request, info->resource);

    if (cupsGetError() >= IPP_STATUS_REDIRECTION_OTHER_SITE)
        plogf(pjob, "Unable to update the printer subscriptions : %s", cupsGetErrorString());

    return (void*)response;

}
static void* Update_active_jobs(proxy_info_t *info, proxy_job_t *pjob) {
    ipp_t	*request,			/* IPP request */
            *response;


    request = ippNewRequest(IPP_OP_UPDATE_ACTIVE_JOBS);

    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->printer_uri);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL,info->device_uuid); //Test:cloud-device-id
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());
    
    ippAddIntegers(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-ids", (int)(sizeof(local_pending_jobs) / sizeof(local_pending_jobs[0])), local_pending_jobs);
    ippAddIntegers(request, IPP_TAG_OPERATION, IPP_TAG_ENUM, "output-device-job-states", (int)(sizeof(local_pending_job_states) / sizeof(local_pending_job_states[0])), local_pending_job_states);

    response = cupsDoRequest(info->http, request, info->resource);
    print_conn_info(response,"Update_active_jobs");
    ippDelete(response);
    const char		*message;	/* Document format */
    if ((message = ippGetString(ippFindAttribute(response, "detailed-status-message", IPP_TAG_TEXT), 0, NULL)) == NULL){
            plogf(pjob, "Update_active_jobs message: %s", message);
    } else 
    {
            plogf(pjob, "Update_active_jobs detailed message: %s", message);
    }

    if (cupsGetError() >= IPP_STATUS_REDIRECTION_OTHER_SITE)
    plogf(NULL, "Unable to update active job subscription: %s", cupsGetErrorString());
   // return (void*)response;
}
static void* print_conn_info(ipp_t* http,char* func){
    ipp_attribute_t	*attr;		/* IPP attribute */
    plogf(NULL,"print_conn_info func:%s",func);
    const char		*name;		/* Attribute name */
    for (attr = ippGetFirstAttribute(http); attr; attr = ippGetNextAttribute(http)){
        name = ippGetName(attr);
        //printf("attr:%s \n",name);
        //plogf(NULL,"attr:%s GroupTag:%d ",name,ippGetGroupTag(attr));
    }
}
static void* process_pending_in_queue_jobs(proxy_info_t *info, proxy_job_t *pjob){
 Update_active_jobs(info,pjob);
}

static void*	create_job_subscriptions(proxy_info_t *info, 
        proxy_job_t *pjob)
{
    ipp_t	*request,			/* IPP request */
            *response;


    request = ippNewRequest(IPP_OP_CREATE_JOB_SUBSCRIPTIONS);

    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, info->printer_uri);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsGetUser());
    ippAddInteger(request, IPP_TAG_SUBSCRIPTION, IPP_TAG_INTEGER, "notify-job-id", pjob->remote_job_id); //TODO: Fix this remote_job_id issue

    response = cupsDoRequest(info->http, request, info->resource);

    if (cupsGetError() >= IPP_STATUS_REDIRECTION_OTHER_SITE)
        plogf(pjob, "Unable to update the job subscription: %s", cupsGetErrorString());
    return (void*)response;
}

static int					/* O - Number of jobs */
cupsGetJobs_proxy(
    http_t* http,		
    cups_job_t** jobs,
    const char* resource,
    const char* printer_uri,
    const char* device_uuid)
{
    ipp_t* request,		/* IPP Request */
        * response;		/* IPP Response */
    ipp_attribute_t* attr;		/* Current attribute */
    cups_job_t* temp;			/* Temporary pointer */
    int		id;			/* job-id */
    ipp_jstate_t	state;			/* job-state */
    const char    * event, *name;		/* Current event */
    int			job_id;		/* Job ID, if any */
    static const char* const attrs[] =	/* Requested attributes */
    {
      "document-format",
      "job-id",
      //"job-k-octets",
      "job-name",
      "job-originating-user-name",
      "job-printer-uri",
      "job-priority",
      "job-state",
      //"time-at-completed",
      //"time-at-creation",
      //"time-at-processing",
      "job-state-reasons"
    };
    int			seq_number = 1;	/* Current event sequence number */
    ipp_jstate_t		job_state;	/* Job state, if any */
    /*
     * Range check input...
     */

    /*
     * Build an IPP_GET_JOBS request, which requires the following
     * attributes:
     *
     *    attributes-charset
     *    attributes-natural-language
     *    printer-uri
     *    requesting-user-name
     *    which-jobs
     *    my-jobs
     *    requested-attributes
     */

    request = ippNewRequest(IPP_OP_GET_JOBS);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI,"printer-uri", NULL, printer_uri);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "output-device-uuid", NULL, device_uuid);
    ippAddStrings(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD,
        "requested-attributes", sizeof(attrs) / sizeof(attrs[0]),
        NULL, attrs);

    /*
     * Do the request and get back a response...
     */

    *jobs = NULL;
    
    if ((response = cupsDoRequest(http, request, resource)) != NULL)
    {
        print_conn_info(response, "proxy_jobs");

//#if 1
        for (attr = ippGetFirstAttribute(response); attr; attr = ippGetNextAttribute(response))
        {
            if (ippGetGroupTag(attr) != IPP_TAG_JOB || !ippGetName(attr))
                continue;

            event = NULL;
            job_id = 0;
            job_state = IPP_JSTATE_PENDING;
            while (ippGetGroupTag(attr) == IPP_TAG_JOB && (name = ippGetName(attr)) != NULL)
            {
                if (!strcmp(name, "job-state-reasons") && ippGetValueTag(attr) == IPP_TAG_KEYWORD) {
                    event = ippGetString(attr, 0, NULL);
                    plogf(NULL, "job-state-reasons=%s", event);
                }
                else if (!strcmp(name, "job-id") && ippGetValueTag(attr) == IPP_TAG_INTEGER) {
                    job_id = ippGetInteger(attr, 0);
                    plogf(NULL, "job-id=%d", job_id);
                }
                else if (!strcmp(name, "job-state") && ippGetValueTag(attr) == IPP_TAG_ENUM) {
                    job_state = (ipp_jstate_t)ippGetInteger(attr, 0);
                    plogf(NULL, "job-state=%d", (int)job_state);
                }
                else if (!strcmp(name, "notify-sequence-number") && ippGetValueTag(attr) == IPP_TAG_INTEGER)
                {
                    int new_seq = ippGetInteger(attr, 0);

                    if (new_seq >= seq_number)
                        seq_number = new_seq + 1;
                    plogf(NULL, "notify-sequence-number=%d", new_seq);

                }
                else if (!strcmp(name, "printer-state-reasons") && ippContainsString(attr, "identify-printer-requested")) {
                    //acknowledge_identify_printer(http, printer_uri, notifications_uri, resource, device_uuid);
                }

                attr = ippGetNextAttribute(response);
            }

            if (event && job_id)
            {
                if (!strcmp(event, "job-fetchable") && job_id)
                {
                    /*
                     * Queue up new job...
                     */

                    proxy_job_t* pjob = find_job(job_id);

                    if (!pjob)
                    {
                        /*
                         * Not already queued up, make a new one...
                         */

                        if ((pjob = (proxy_job_t*)calloc(1, sizeof(proxy_job_t))) != NULL)
                        {
                            /*
                             * Add job and then let the proxy thread know we added something...
                             */

                            pjob->remote_job_id = job_id;
                            pjob->remote_job_state = (int)job_state;
                            pjob->local_job_state = IPP_JSTATE_PENDING;

                            plogf(pjob, "Job is now fetchable, queuing up.", pjob);

                            cupsRWLockWrite(&jobs_rwlock);
                            cupsArrayAdd(jobs_lst, pjob);
                            cupsRWUnlock(&jobs_rwlock);

                            cupsCondBroadcast(&jobs_cond);
                        }
                        else
                        {
                            plogf(NULL, "Unable to add job %d to jobs queue.", job_id);
                        }
                    }
                }
                else if (!strcmp(event, "job-state-changed") && job_id)
                {
                    /*
                     * Update our cached job info...  If the job is currently being
                     * proxied and the job has been canceled or aborted, the code will see
                     * that and stop printing locally.
                     */

                    proxy_job_t* pjob = find_job(job_id);

                    if (pjob)
                    {
                        pjob->remote_job_state = (int)job_state;

                        plogf(pjob, "Updated remote job-state to '%s'.", ippEnumString("job-state", (int)job_state));

                        cupsCondBroadcast(&jobs_cond);
                    }
                }
            }
        }
        ippDelete(response);
    }
}

/*
 * 'usage()' - Show program usage and exit.
 */

static void
usage(int status)			/* O - Exit status */
{
  puts("Usage: ippproxy [options] printer-uri");
  puts("Options:");
  puts("  -d device-uri   Specify local printer device URI.");
   puts("  -n notifications-uri   Specify remote notifications printer URI.");
  puts("  -m mime/type    Specify the desired print format.");
  puts("  -p password     Password for authentication.");
  puts("                  (Also IPPPROXY_PASSWORD environment variable)");
  puts("  -u username     Username for authentication.");
  puts("  -v              Be verbose.");
  puts("  --help          Show this help.");
  puts("  --version       Show program version.");

  exit(status);
}

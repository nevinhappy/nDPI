/*
 * ndpiReader.c
 *
 * Copyright (C) 2011-15 - ntop.org
 * Copyright (C) 2009-2011 by ipoque GmbH
 * Copyright (C) 2014 - Matteo Bogo <matteo.bogo@gmail.com> (JSON support)
 *
 * nDPI is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * nDPI is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with nDPI.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifdef linux
#define _GNU_SOURCE
#include <sched.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#ifdef WIN32
#include <winsock2.h> /* winsock.h is included automatically */
#include <process.h>
#include <io.h>
#include <getopt.h>
#define getopt getopt____
#else
#include <unistd.h>
#include <netinet/in.h>
#endif
#include <string.h>
#include <stdarg.h>
#include <search.h>
#include <pcap.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>

#include "../config.h"
#include "ndpi_api.h"

#ifdef HAVE_JSON_C
#include <json.h>
#endif

#define MAX_NUM_READER_THREADS     16
#define IDLE_SCAN_PERIOD           10 /* msec (use detection_tick_resolution = 1000) */
#define MAX_IDLE_TIME           30000
#define IDLE_SCAN_BUDGET         1024
#define NUM_ROOTS                 512
#define MAX_NDPI_FLOWS      200000000

#include "ndpi_util.h"

/**
 * @brief Set main components necessary to the detection
 * @details TODO
 */
static void setupDetection(u_int16_t thread_id, pcap_t * pcap_handle);

/**
 * Client parameters
 */
static char *_pcap_file[MAX_NUM_READER_THREADS]; /**< Ingress pcap file/interafaces */
static FILE *playlist_fp[MAX_NUM_READER_THREADS] = { NULL }; /**< Ingress playlist */
static FILE *results_file = NULL;
static char *results_path = NULL;
static char *_bpf_filter      = NULL; /**< bpf filter  */
static char *_protoFilePath   = NULL; /**< Protocol file path  */
#ifdef HAVE_JSON_C
static char *_jsonFilePath    = NULL; /**< JSON file path  */
#endif
#ifdef HAVE_JSON_C
static json_object *jArray_known_flows, *jArray_unknown_flows;
#endif
static u_int8_t live_capture = 0;
static u_int8_t undetected_flows_deleted = 0;
/**
 * User preferences
 */
static u_int8_t enable_protocol_guess = 1, verbose = 0, nDPI_traceLevel = 0, json_flag = 0;
static u_int16_t decode_tunnels = 0;
static u_int16_t num_loops = 1;
static u_int8_t shutdown_app = 0, quiet_mode = 0;
static u_int8_t num_threads = 1;
#ifdef linux
static int core_affinity[MAX_NUM_READER_THREADS];
#endif

static struct timeval pcap_start, pcap_end;

/**
 * Detection parameters
 */
static u_int32_t detection_tick_resolution = 1000;
static time_t capture_for = 0;
static time_t capture_until = 0;

static u_int32_t num_flows;

struct reader_thread {
  struct ndpi_workflow * workflow;
  pthread_t pthread;
};

static struct reader_thread ndpi_thread_info[MAX_NUM_READER_THREADS];

/**
 * @brief ID tracking
 */
typedef struct ndpi_id {
  u_int8_t ip[4];				// Ip address
  struct ndpi_id_struct *ndpi_id;		// nDpi worker structure
} ndpi_id_t;

static u_int32_t size_id_struct = 0;		// ID tracking structure size


static u_int32_t size_flow_struct = 0;

static void help(u_int long_help) {
  printf("ndpiReader -i <file|device> [-f <filter>][-s <duration>]\n"
	 "          [-p <protos>][-l <loops> [-q][-d][-h][-t][-v <level>]\n"
	 "          [-n <threads>] [-w <file>] [-j <file>]\n\n"
	 "Usage:\n"
	 "  -i <file.pcap|device>     | Specify a pcap file/playlist to read packets from or a device for live capture (comma-separated list)\n"
	 "  -f <BPF filter>           | Specify a BPF filter for filtering selected traffic\n"
	 "  -s <duration>             | Maximum capture duration in seconds (live traffic capture only)\n"
	 "  -p <file>.protos          | Specify a protocol file (eg. protos.txt)\n"
	 "  -l <num loops>            | Number of detection loops (test only)\n"
	 "  -n <num threads>          | Number of threads. Default: number of interfaces in -i. Ignored with pcap files.\n"
	 "  -j <file.json>            | Specify a file to write the content of packets in .json format\n"
#ifdef linux
         "  -g <id:id...>             | Thread affinity mask (one core id per thread)\n"
#endif
	 "  -d                        | Disable protocol guess and use only DPI\n"
	 "  -q                        | Quiet mode\n"
	 "  -t                        | Dissect GTP/TZSP tunnels\n"
	 "  -r                        | Print nDPI version and git revision\n"
	 "  -w <path>                 | Write test output on the specified file. This is useful for\n"
	 "                            | testing purposes in order to compare results across runs\n"
	 "  -h                        | This help\n"
	 "  -v <1|2>                  | Verbose 'unknown protocol' packet print. 1=verbose, 2=very verbose\n");

  if(long_help) {
    printf("\n\nSupported protocols:\n");
    num_threads = 1;
    setupDetection(0, NULL);
    ndpi_dump_protocols(ndpi_thread_info[0].workflow->ndpi_struct);
  }

  exit(!long_help);
}

/* ***************************************************** */

static void parseOptions(int argc, char **argv) {
  char *__pcap_file = NULL, *bind_mask = NULL;
  int thread_id, opt;
#ifdef linux
  u_int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
#endif

  while ((opt = getopt(argc, argv, "df:g:i:hp:l:s:tv:V:n:j:rp:w:q")) != EOF) {
    switch (opt) {
    case 'd':
      enable_protocol_guess = 0;
      break;

    case 'i':
      _pcap_file[0] = optarg;
      break;

    case 'f':
      _bpf_filter = optarg;
      break;

    case 'g':
      bind_mask = optarg;
      break;

    case 'l':
      num_loops = atoi(optarg);
      break;

    case 'n':
      num_threads = atoi(optarg);
      break;

    case 'p':
      _protoFilePath = optarg;
      break;

    case 's':
      capture_for = atoi(optarg);
      capture_until = capture_for + time(NULL);
      break;

    case 't':
      decode_tunnels = 1;
      break;

    case 'r':
      printf("ndpiReader - nDPI (%s)\n", ndpi_revision());
      exit(0);

    case 'v':
      verbose = atoi(optarg);
      break;

    case 'V':
      printf("%d\n",atoi(optarg) );
      nDPI_traceLevel  = atoi(optarg);
      break;

    case 'h':
      help(1);
      break;

    case 'j':
#ifndef HAVE_JSON_C
      printf("WARNING: this copy of ndpiReader has been compiled without JSON-C: json export disabled\n");
#else
      _jsonFilePath = optarg;
      json_flag = 1;
#endif
      break;

    case 'w':
      results_path = strdup(optarg);
      if((results_file = fopen(results_path, "w")) == NULL) {
	printf("Unable to write in file %s: quitting\n", results_path);
	return;
      }
      break;

    case 'q':
      quiet_mode = 1;
      break;

    default:
      help(0);
      break;
    }
  }

  // check parameters
  if(_pcap_file[0] == NULL || strcmp(_pcap_file[0], "") == 0) {
    help(0);
  }

  if(strchr(_pcap_file[0], ',')) { /* multiple ingress interfaces */
    num_threads = 0; /* setting number of threads = number of interfaces */
    __pcap_file = strtok(_pcap_file[0], ",");
    while (__pcap_file != NULL && num_threads < MAX_NUM_READER_THREADS) {
      _pcap_file[num_threads++] = __pcap_file;
      __pcap_file = strtok(NULL, ",");
    }
  } else {
    if(num_threads > MAX_NUM_READER_THREADS) num_threads = MAX_NUM_READER_THREADS;
    for(thread_id = 1; thread_id < num_threads; thread_id++)
      _pcap_file[thread_id] = _pcap_file[0];
  }

#ifdef linux
  for(thread_id = 0; thread_id < num_threads; thread_id++)
    core_affinity[thread_id] = -1;

  if(num_cores > 1 && bind_mask != NULL) {
    char *core_id = strtok(bind_mask, ":");
    thread_id = 0;
    while (core_id != NULL && thread_id < num_threads) {
      core_affinity[thread_id++] = atoi(core_id) % num_cores;
      core_id = strtok(NULL, ":");
    }
  }
#endif
}

static char* ipProto2Name(u_short proto_id) {
  static char proto[8];

  switch(proto_id) {
  case IPPROTO_TCP:
    return("TCP");
    break;
  case IPPROTO_UDP:
    return("UDP");
    break;
  case IPPROTO_ICMP:
    return("ICMP");
    break;
  case IPPROTO_ICMPV6:
    return("ICMPV6");
    break;
  case 112:
    return("VRRP");
    break;
  case IPPROTO_IGMP:
    return("IGMP");
    break;
  }

  snprintf(proto, sizeof(proto), "%u", proto_id);
  return(proto);
}

/* ***************************************************** */

/*
 * A faster replacement for inet_ntoa().
 */
char* intoaV4(unsigned int addr, char* buf, u_short bufLen) {
  char *cp, *retStr;
  uint byte;
  int n;

  cp = &buf[bufLen];
  *--cp = '\0';

  n = 4;
  do {
    byte = addr & 0xff;
    *--cp = byte % 10 + '0';
    byte /= 10;
    if(byte > 0) {
      *--cp = byte % 10 + '0';
      byte /= 10;
      if(byte > 0)
	*--cp = byte + '0';
    }
    *--cp = '.';
    addr >>= 8;
  } while (--n > 0);

  /* Convert the string to lowercase */
  retStr = (char*)(cp+1);

  return(retStr);
}

/* ***************************************************** */
static u_int32_t current_ndpi_memory = 0, max_ndpi_memory = 0;

static void *malloc_wrapper(size_t size) {
  current_ndpi_memory += size;

  if(current_ndpi_memory > max_ndpi_memory)
    max_ndpi_memory = current_ndpi_memory;

  return malloc(size);
}

/* ***************************************************** */

static void free_wrapper(void *freeable) {
  free(freeable);
}

/* ***************************************************** */

static void printFlow(u_int16_t thread_id, struct ndpi_flow_info *flow) {
#ifdef HAVE_JSON_C
  json_object *jObj;
#endif
  FILE *out = results_file ? results_file : stdout;

  if(!json_flag) {
    fprintf(out, "\t%u", ++num_flows);

    fprintf(out, "\t%s %s%s%s:%u <-> %s%s%s:%u ",
	    ipProto2Name(flow->protocol),
	    (flow->ip_version == 6) ? "[" : "",
	    flow->lower_name, 
	    (flow->ip_version == 6) ? "]" : "",
	    ntohs(flow->lower_port),
	    (flow->ip_version == 6) ? "[" : "",
	    flow->upper_name, 
	    (flow->ip_version == 6) ? "]" : "",
	    ntohs(flow->upper_port));

    if(flow->vlan_id > 0) fprintf(out, "[VLAN: %u]", flow->vlan_id);

    if(flow->detected_protocol.master_protocol) {
      char buf[64];

      fprintf(out, "[proto: %u.%u/%s]",
	      flow->detected_protocol.master_protocol, flow->detected_protocol.protocol,
	      ndpi_protocol2name(ndpi_thread_info[thread_id].workflow->ndpi_struct,
				 flow->detected_protocol, buf, sizeof(buf)));
    } else
      fprintf(out, "[proto: %u/%s]",
	      flow->detected_protocol.protocol,
	      ndpi_get_proto_name(ndpi_thread_info[thread_id].workflow->ndpi_struct, flow->detected_protocol.protocol));

    fprintf(out, "[%u pkts/%llu bytes]",
	    flow->packets, (long long unsigned int)flow->bytes);

    if(flow->host_server_name[0] != '\0') fprintf(out, "[Host: %s]", flow->host_server_name);
    if(flow->ssl.client_certificate[0] != '\0') fprintf(out, "[SSL client: %s]", flow->ssl.client_certificate);
    if(flow->ssl.server_certificate[0] != '\0') fprintf(out, "[SSL server: %s]", flow->ssl.server_certificate);
    if(flow->bittorent_hash[0] != '\0') fprintf(out, "[BT Hash: %s]", flow->bittorent_hash);

    fprintf(out, "\n");
  } else {
#ifdef HAVE_JSON_C
    jObj = json_object_new_object();

    json_object_object_add(jObj,"protocol",json_object_new_string(ipProto2Name(flow->protocol)));
    json_object_object_add(jObj,"host_a.name",json_object_new_string(flow->lower_name));
    json_object_object_add(jObj,"host_a.port",json_object_new_int(ntohs(flow->lower_port)));
    json_object_object_add(jObj,"host_b.name",json_object_new_string(flow->upper_name));
    json_object_object_add(jObj,"host_b.port",json_object_new_int(ntohs(flow->upper_port)));

    if(flow->detected_protocol.master_protocol)
      json_object_object_add(jObj,"detected.masterprotocol",json_object_new_int(flow->detected_protocol.master_protocol));

    json_object_object_add(jObj,"detected.protocol",json_object_new_int(flow->detected_protocol.protocol));

    if(flow->detected_protocol.master_protocol) {
      char tmp[256];

      snprintf(tmp, sizeof(tmp), "%s.%s",
	       ndpi_get_proto_name(ndpi_thread_info[thread_id].workflow->ndpi_struct, flow->detected_protocol.master_protocol),
	       ndpi_get_proto_name(ndpi_thread_info[thread_id].workflow->ndpi_struct, flow->detected_protocol.protocol));

      json_object_object_add(jObj,"detected.protocol.name",
			     json_object_new_string(tmp));
    } else
      json_object_object_add(jObj,"detected.protocol.name",
			     json_object_new_string(ndpi_get_proto_name(ndpi_thread_info[thread_id].workflow->ndpi_struct,
									flow->detected_protocol.protocol)));

    json_object_object_add(jObj,"packets",json_object_new_int(flow->packets));
    json_object_object_add(jObj,"bytes",json_object_new_int(flow->bytes));

    if(flow->host_server_name[0] != '\0')
      json_object_object_add(jObj,"host.server.name",json_object_new_string(flow->host_server_name));

    if((flow->ssl.client_certificate[0] != '\0') || (flow->ssl.server_certificate[0] != '\0')) {
      json_object *sjObj = json_object_new_object();

      if(flow->ssl.client_certificate[0] != '\0')
	json_object_object_add(sjObj, "client", json_object_new_string(flow->ssl.client_certificate));

      if(flow->ssl.server_certificate[0] != '\0')
	json_object_object_add(sjObj, "server", json_object_new_string(flow->ssl.server_certificate));

      json_object_object_add(jObj, "ssl", sjObj);
    }

    //flow->protos.ssl.client_certificate, flow->protos.ssl.server_certificate);
    if(json_flag == 1)
      json_object_array_add(jArray_known_flows,jObj);
    else if(json_flag == 2)
      json_object_array_add(jArray_unknown_flows,jObj);
#endif
  }
}

/* ***************************************************** */

static void free_ndpi_flow_info(struct ndpi_flow_info *flow) {
  if(flow->ndpi_flow) { ndpi_free_flow(flow->ndpi_flow); flow->ndpi_flow = NULL; }
  if(flow->src_id)    { ndpi_free(flow->src_id); flow->src_id = NULL; }
  if(flow->dst_id)    { ndpi_free(flow->dst_id); flow->dst_id = NULL; }

}

/* ***************************************************** */

static void ndpi_flow_info_freer(void *node) {
  struct ndpi_flow_info *flow = (struct ndpi_flow_info*)node;

  free_ndpi_flow_info(flow);
  ndpi_free(flow);
}

/* ***************************************************** */

static void node_print_unknown_proto_walker(const void *node, ndpi_VISIT which, int depth, void *user_data) {
  struct ndpi_flow_info *flow = *(struct ndpi_flow_info**)node;
  u_int16_t thread_id = *((u_int16_t*)user_data);

  if(flow->detected_protocol.protocol != NDPI_PROTOCOL_UNKNOWN) return;

  if((which == ndpi_preorder) || (which == ndpi_leaf)) /* Avoid walking the same node multiple times */
    printFlow(thread_id, flow);
}

/* ***************************************************** */

static void node_print_known_proto_walker(const void *node, ndpi_VISIT which, int depth, void *user_data) {
  struct ndpi_flow_info *flow = *(struct ndpi_flow_info**)node;
  u_int16_t thread_id = *((u_int16_t*)user_data);

  if(flow->detected_protocol.protocol == NDPI_PROTOCOL_UNKNOWN) return;

  if((which == ndpi_preorder) || (which == ndpi_leaf)) /* Avoid walking the same node multiple times */
    printFlow(thread_id, flow);
}

/* ***************************************************** */

static u_int16_t node_guess_undetected_protocol(u_int16_t thread_id, struct ndpi_flow_info *flow) {
  flow->detected_protocol = ndpi_guess_undetected_protocol(ndpi_thread_info[thread_id].workflow->ndpi_struct,
							   flow->protocol,
							   ntohl(flow->lower_ip),
							   ntohs(flow->lower_port),
							   ntohl(flow->upper_ip),
							   ntohs(flow->upper_port));
  // printf("Guess state: %u\n", flow->detected_protocol);
  if(flow->detected_protocol.protocol != NDPI_PROTOCOL_UNKNOWN)
    ndpi_thread_info[thread_id].workflow->stats.guessed_flow_protocols++;

  return(flow->detected_protocol.protocol);
}

/* ***************************************************** */

static void node_proto_guess_walker(const void *node, ndpi_VISIT which, int depth, void *user_data) {
  struct ndpi_flow_info *flow = *(struct ndpi_flow_info **) node;
  u_int16_t thread_id = *((u_int16_t *) user_data);

  if((which == ndpi_preorder) || (which == ndpi_leaf)) { /* Avoid walking the same node multiple times */
    if((!flow->detection_completed) && flow->ndpi_flow)
      flow->detected_protocol = ndpi_detection_giveup(ndpi_thread_info[0].workflow->ndpi_struct, flow->ndpi_flow);

    if(enable_protocol_guess) {
      if(flow->detected_protocol.protocol == NDPI_PROTOCOL_UNKNOWN) {
	node_guess_undetected_protocol(thread_id, flow);
	// printFlow(thread_id, flow);
      }
    }

    ndpi_thread_info[thread_id].workflow->stats.protocol_counter[flow->detected_protocol.protocol]       += flow->packets;
    ndpi_thread_info[thread_id].workflow->stats.protocol_counter_bytes[flow->detected_protocol.protocol] += flow->bytes;
    ndpi_thread_info[thread_id].workflow->stats.protocol_flows[flow->detected_protocol.protocol]++;
  }
}

/* ***************************************************** */

static void node_idle_scan_walker(const void *node, ndpi_VISIT which, int depth, void *user_data) {
  struct ndpi_flow_info *flow = *(struct ndpi_flow_info **) node;
  u_int16_t thread_id = *((u_int16_t *) user_data);

  if(ndpi_thread_info[thread_id].workflow->num_idle_flows == IDLE_SCAN_BUDGET) /* TODO optimise with a budget-based walk */
    return;

  if((which == ndpi_preorder) || (which == ndpi_leaf)) { /* Avoid walking the same node multiple times */
    if(flow->last_seen + MAX_IDLE_TIME < ndpi_thread_info[thread_id].workflow->last_time) {

      /* update stats */
      node_proto_guess_walker(node, which, depth, user_data);

      if((flow->detected_protocol.protocol == NDPI_PROTOCOL_UNKNOWN) && !undetected_flows_deleted)
        undetected_flows_deleted = 1;

      free_ndpi_flow_info(flow);
      ndpi_thread_info[thread_id].workflow->stats.ndpi_flow_count--;

      /* adding to a queue (we can't delete it from the tree inline ) */
      ndpi_thread_info[thread_id].workflow->idle_flows[ndpi_thread_info[thread_id].workflow->num_idle_flows++] = flow;
    }
  }
}

/* ***************************************************** */

static void setupDetection(u_int16_t thread_id, pcap_t * pcap_handle) {
  NDPI_PROTOCOL_BITMASK all;
  
  struct ndpi_workflow_prefs prefs;
  memset(&prefs, 0, sizeof(prefs));
  prefs.decode_tunnels = decode_tunnels;
  prefs.num_roots = NUM_ROOTS;
  prefs.max_ndpi_flows = MAX_NDPI_FLOWS;
  prefs.quiet_mode = quiet_mode;
  prefs.detection_tick_resolution = detection_tick_resolution;

  memset(&ndpi_thread_info[thread_id], 0, sizeof(ndpi_thread_info[thread_id]));
  ndpi_thread_info[thread_id].workflow = ndpi_workflow_init(&prefs, pcap_handle, malloc_wrapper, free_wrapper);
  /* ndpi_thread_info[thread_id].workflow->ndpi_struct->http_dont_dissect_response = 1; */

  // enable all protocols
  NDPI_BITMASK_SET_ALL(all);
  ndpi_set_protocol_detection_bitmask2(ndpi_thread_info[thread_id].workflow->ndpi_struct, &all);

  // clear memory for results
  memset(ndpi_thread_info[thread_id].workflow->stats.protocol_counter, 0, sizeof(ndpi_thread_info[thread_id].workflow->stats.protocol_counter));
  memset(ndpi_thread_info[thread_id].workflow->stats.protocol_counter_bytes, 0, sizeof(ndpi_thread_info[thread_id].workflow->stats.protocol_counter_bytes));
  memset(ndpi_thread_info[thread_id].workflow->stats.protocol_flows, 0, sizeof(ndpi_thread_info[thread_id].workflow->stats.protocol_flows));

  if(_protoFilePath != NULL)
    ndpi_load_protocols_file(ndpi_thread_info[thread_id].workflow->ndpi_struct, _protoFilePath);
}

/* ***************************************************** */

static void terminateDetection(u_int16_t thread_id) {
  int i;

  for(i=0; i<NUM_ROOTS; i++) {
    ndpi_tdestroy(ndpi_thread_info[thread_id].workflow->ndpi_flows_root[i], ndpi_flow_info_freer);
    ndpi_thread_info[thread_id].workflow->ndpi_flows_root[i] = NULL;
  }

  ndpi_workflow_free(ndpi_thread_info[thread_id].workflow);
}

char* formatTraffic(float numBits, int bits, char *buf) {
  char unit;

  if(bits)
    unit = 'b';
  else
    unit = 'B';

  if(numBits < 1024) {
    snprintf(buf, 32, "%lu %c", (unsigned long)numBits, unit);
  } else if(numBits < 1048576) {
    snprintf(buf, 32, "%.2f K%c", (float)(numBits)/1024, unit);
  } else {
    float tmpMBits = ((float)numBits)/1048576;

    if(tmpMBits < 1024) {
      snprintf(buf, 32, "%.2f M%c", tmpMBits, unit);
    } else {
      tmpMBits /= 1024;

      if(tmpMBits < 1024) {
	snprintf(buf, 32, "%.2f G%c", tmpMBits, unit);
      } else {
	snprintf(buf, 32, "%.2f T%c", (float)(tmpMBits)/1024, unit);
      }
    }
  }

  return(buf);
}

/* ***************************************************** */

char* formatPackets(float numPkts, char *buf) {
  if(numPkts < 1000) {
    snprintf(buf, 32, "%.2f", numPkts);
  } else if(numPkts < 1000000) {
    snprintf(buf, 32, "%.2f K", numPkts/1000);
  } else {
    numPkts /= 1000000;
    snprintf(buf, 32, "%.2f M", numPkts);
  }

  return(buf);
}

/* ***************************************************** */

#ifdef HAVE_JSON_C
static void json_init() {
  jArray_known_flows = json_object_new_array();
  jArray_unknown_flows = json_object_new_array();
}
#endif

/* ***************************************************** */

char* formatBytes(u_int32_t howMuch, char *buf, u_int buf_len) {
  char unit = 'B';

  if(howMuch < 1024) {
    snprintf(buf, buf_len, "%lu %c", (unsigned long)howMuch, unit);
  } else if(howMuch < 1048576) {
    snprintf(buf, buf_len, "%.2f K%c", (float)(howMuch)/1024, unit);
  } else {
    float tmpGB = ((float)howMuch)/1048576;

    if(tmpGB < 1024) {
      snprintf(buf, buf_len, "%.2f M%c", tmpGB, unit);
    } else {
      tmpGB /= 1024;

      snprintf(buf, buf_len, "%.2f G%c", tmpGB, unit);
    }
  }

  return(buf);
}

/* ***************************************************** */

static void printResults(u_int64_t tot_usec) {
  u_int32_t i;
  u_int64_t total_flow_bytes = 0;
  u_int avg_pkt_size = 0;
  struct ndpi_stats cumulative_stats;
  int thread_id;
  char buf[32];
#ifdef HAVE_JSON_C
  FILE *json_fp = NULL;
  json_object *jObj_main, *jObj_trafficStats, *jArray_detProto, *jObj;
#endif
  long long unsigned int breed_stats[NUM_BREEDS] = { 0 };

  memset(&cumulative_stats, 0, sizeof(cumulative_stats));

  for(thread_id = 0; thread_id < num_threads; thread_id++) {
    if(ndpi_thread_info[thread_id].workflow->stats.total_wire_bytes == 0) continue;

    for(i=0; i<NUM_ROOTS; i++)
      ndpi_twalk(ndpi_thread_info[thread_id].workflow->ndpi_flows_root[i], node_proto_guess_walker, &thread_id);

    /* Stats aggregation */
    cumulative_stats.guessed_flow_protocols += ndpi_thread_info[thread_id].workflow->stats.guessed_flow_protocols;
    cumulative_stats.raw_packet_count += ndpi_thread_info[thread_id].workflow->stats.raw_packet_count;
    cumulative_stats.ip_packet_count += ndpi_thread_info[thread_id].workflow->stats.ip_packet_count;
    cumulative_stats.total_wire_bytes += ndpi_thread_info[thread_id].workflow->stats.total_wire_bytes;
    cumulative_stats.total_ip_bytes += ndpi_thread_info[thread_id].workflow->stats.total_ip_bytes;
    cumulative_stats.total_discarded_bytes += ndpi_thread_info[thread_id].workflow->stats.total_discarded_bytes;

    for(i = 0; i < ndpi_get_num_supported_protocols(ndpi_thread_info[0].workflow->ndpi_struct); i++) {
      cumulative_stats.protocol_counter[i] += ndpi_thread_info[thread_id].workflow->stats.protocol_counter[i];
      cumulative_stats.protocol_counter_bytes[i] += ndpi_thread_info[thread_id].workflow->stats.protocol_counter_bytes[i];
      cumulative_stats.protocol_flows[i] += ndpi_thread_info[thread_id].workflow->stats.protocol_flows[i];
    }

    cumulative_stats.ndpi_flow_count += ndpi_thread_info[thread_id].workflow->stats.ndpi_flow_count;
    cumulative_stats.tcp_count   += ndpi_thread_info[thread_id].workflow->stats.tcp_count;
    cumulative_stats.udp_count   += ndpi_thread_info[thread_id].workflow->stats.udp_count;
    cumulative_stats.mpls_count  += ndpi_thread_info[thread_id].workflow->stats.mpls_count;
    cumulative_stats.pppoe_count += ndpi_thread_info[thread_id].workflow->stats.pppoe_count;
    cumulative_stats.vlan_count  += ndpi_thread_info[thread_id].workflow->stats.vlan_count;
    cumulative_stats.fragmented_count += ndpi_thread_info[thread_id].workflow->stats.fragmented_count;
    for(i = 0; i < 6; i++)
      cumulative_stats.packet_len[i] += ndpi_thread_info[thread_id].workflow->stats.packet_len[i];
    cumulative_stats.max_packet_len += ndpi_thread_info[thread_id].workflow->stats.max_packet_len;
  }

  if(!quiet_mode) {
    printf("\nnDPI Memory statistics:\n");
    printf("\tnDPI Memory (once):      %-13s\n", formatBytes(sizeof(struct ndpi_detection_module_struct), buf, sizeof(buf)));
    printf("\tFlow Memory (per flow):  %-13s\n", formatBytes(size_flow_struct, buf, sizeof(buf)));
    printf("\tActual Memory:           %-13s\n", formatBytes(current_ndpi_memory, buf, sizeof(buf)));
    printf("\tPeak Memory:             %-13s\n", formatBytes(max_ndpi_memory, buf, sizeof(buf)));

    if(!json_flag) {
      printf("\nTraffic statistics:\n");
      printf("\tEthernet bytes:        %-13llu (includes ethernet CRC/IFC/trailer)\n",
	     (long long unsigned int)cumulative_stats.total_wire_bytes);
      printf("\tDiscarded bytes:       %-13llu\n",
	     (long long unsigned int)cumulative_stats.total_discarded_bytes);
      printf("\tIP packets:            %-13llu of %llu packets total\n",
	     (long long unsigned int)cumulative_stats.ip_packet_count,
	     (long long unsigned int)cumulative_stats.raw_packet_count);
      /* In order to prevent Floating point exception in case of no traffic*/
      if(cumulative_stats.total_ip_bytes && cumulative_stats.raw_packet_count)
	avg_pkt_size = (unsigned int)(cumulative_stats.total_ip_bytes/cumulative_stats.raw_packet_count);
      printf("\tIP bytes:              %-13llu (avg pkt size %u bytes)\n",
	     (long long unsigned int)cumulative_stats.total_ip_bytes,avg_pkt_size);
      printf("\tUnique flows:          %-13u\n", cumulative_stats.ndpi_flow_count);

      printf("\tTCP Packets:           %-13lu\n", (unsigned long)cumulative_stats.tcp_count);
      printf("\tUDP Packets:           %-13lu\n", (unsigned long)cumulative_stats.udp_count);
      printf("\tVLAN Packets:          %-13lu\n", (unsigned long)cumulative_stats.vlan_count);
      printf("\tMPLS Packets:          %-13lu\n", (unsigned long)cumulative_stats.mpls_count);
      printf("\tPPPoE Packets:         %-13lu\n", (unsigned long)cumulative_stats.pppoe_count);
      printf("\tFragmented Packets:    %-13lu\n", (unsigned long)cumulative_stats.fragmented_count);
      printf("\tMax Packet size:       %-13u\n",   cumulative_stats.max_packet_len);
      printf("\tPacket Len < 64:       %-13lu\n", (unsigned long)cumulative_stats.packet_len[0]);
      printf("\tPacket Len 64-128:     %-13lu\n", (unsigned long)cumulative_stats.packet_len[1]);
      printf("\tPacket Len 128-256:    %-13lu\n", (unsigned long)cumulative_stats.packet_len[2]);
      printf("\tPacket Len 256-1024:   %-13lu\n", (unsigned long)cumulative_stats.packet_len[3]);
      printf("\tPacket Len 1024-1500:  %-13lu\n", (unsigned long)cumulative_stats.packet_len[4]);
      printf("\tPacket Len > 1500:     %-13lu\n", (unsigned long)cumulative_stats.packet_len[5]);

      if(tot_usec > 0) {
	char buf[32], buf1[32];
	float t = (float)(cumulative_stats.ip_packet_count*1000000)/(float)tot_usec;
	float b = (float)(cumulative_stats.total_wire_bytes * 8 *1000000)/(float)tot_usec;
	float traffic_duration;
	if (live_capture) traffic_duration = tot_usec;
	else traffic_duration = (pcap_end.tv_sec*1000000 + pcap_end.tv_usec) - (pcap_start.tv_sec*1000000 + pcap_start.tv_usec);
	printf("\tnDPI throughput:       %s pps / %s/sec\n", formatPackets(t, buf), formatTraffic(b, 1, buf1));
	t = (float)(cumulative_stats.ip_packet_count*1000000)/(float)traffic_duration;
	b = (float)(cumulative_stats.total_wire_bytes * 8 *1000000)/(float)traffic_duration;
	printf("\tTraffic throughput:    %s pps / %s/sec\n", formatPackets(t, buf), formatTraffic(b, 1, buf1));
	printf("\tTraffic duration:      %.3f sec\n", traffic_duration/1000000);
      }

      if(enable_protocol_guess)
	printf("\tGuessed flow protos:   %-13u\n", cumulative_stats.guessed_flow_protocols);
    }
  }

  if(json_flag) {
#ifdef HAVE_JSON_C
    if((json_fp = fopen(_jsonFilePath,"w")) == NULL) {
      printf("Error createing .json file %s\n", _jsonFilePath);
      json_flag = 0;
    } else {
      jObj_main = json_object_new_object();
      jObj_trafficStats = json_object_new_object();
      jArray_detProto = json_object_new_array();

      json_object_object_add(jObj_trafficStats,"ethernet.bytes",json_object_new_int64(cumulative_stats.total_wire_bytes));
      json_object_object_add(jObj_trafficStats,"discarded.bytes",json_object_new_int64(cumulative_stats.total_discarded_bytes));
      json_object_object_add(jObj_trafficStats,"ip.packets",json_object_new_int64(cumulative_stats.ip_packet_count));
      json_object_object_add(jObj_trafficStats,"total.packets",json_object_new_int64(cumulative_stats.raw_packet_count));
      json_object_object_add(jObj_trafficStats,"ip.bytes",json_object_new_int64(cumulative_stats.total_ip_bytes));
      json_object_object_add(jObj_trafficStats,"avg.pkt.size",json_object_new_int(cumulative_stats.total_ip_bytes/cumulative_stats.raw_packet_count));
      json_object_object_add(jObj_trafficStats,"unique.flows",json_object_new_int(cumulative_stats.ndpi_flow_count));
      json_object_object_add(jObj_trafficStats,"tcp.pkts",json_object_new_int64(cumulative_stats.tcp_count));
      json_object_object_add(jObj_trafficStats,"udp.pkts",json_object_new_int64(cumulative_stats.udp_count));
      json_object_object_add(jObj_trafficStats,"vlan.pkts",json_object_new_int64(cumulative_stats.vlan_count));
      json_object_object_add(jObj_trafficStats,"mpls.pkts",json_object_new_int64(cumulative_stats.mpls_count));
      json_object_object_add(jObj_trafficStats,"pppoe.pkts",json_object_new_int64(cumulative_stats.pppoe_count));
      json_object_object_add(jObj_trafficStats,"fragmented.pkts",json_object_new_int64(cumulative_stats.fragmented_count));
      json_object_object_add(jObj_trafficStats,"max.pkt.size",json_object_new_int(cumulative_stats.max_packet_len));
      json_object_object_add(jObj_trafficStats,"pkt.len_min64",json_object_new_int64(cumulative_stats.packet_len[0]));
      json_object_object_add(jObj_trafficStats,"pkt.len_64_128",json_object_new_int64(cumulative_stats.packet_len[1]));
      json_object_object_add(jObj_trafficStats,"pkt.len_128_256",json_object_new_int64(cumulative_stats.packet_len[2]));
      json_object_object_add(jObj_trafficStats,"pkt.len_256_1024",json_object_new_int64(cumulative_stats.packet_len[3]));
      json_object_object_add(jObj_trafficStats,"pkt.len_1024_1500",json_object_new_int64(cumulative_stats.packet_len[4]));
      json_object_object_add(jObj_trafficStats,"pkt.len_grt1500",json_object_new_int64(cumulative_stats.packet_len[5]));
      json_object_object_add(jObj_trafficStats,"guessed.flow.protos",json_object_new_int(cumulative_stats.guessed_flow_protocols));

      json_object_object_add(jObj_main,"traffic.statistics",jObj_trafficStats);
    }
#endif
  }

  if((!json_flag) && (!quiet_mode)) printf("\n\nDetected protocols:\n");
  for(i = 0; i <= ndpi_get_num_supported_protocols(ndpi_thread_info[0].workflow->ndpi_struct); i++) {
    ndpi_protocol_breed_t breed = ndpi_get_proto_breed(ndpi_thread_info[0].workflow->ndpi_struct, i);

    if(cumulative_stats.protocol_counter[i] > 0) {
      breed_stats[breed] += (long long unsigned int)cumulative_stats.protocol_counter_bytes[i];

      if(results_file)
	fprintf(results_file, "%s\t%llu\t%llu\t%u\n",
		ndpi_get_proto_name(ndpi_thread_info[0].workflow->ndpi_struct, i),
		(long long unsigned int)cumulative_stats.protocol_counter[i],
		(long long unsigned int)cumulative_stats.protocol_counter_bytes[i],
		cumulative_stats.protocol_flows[i]);

      if((!json_flag) && (!quiet_mode)) {
	printf("\t%-20s packets: %-13llu bytes: %-13llu "
	       "flows: %-13u\n",
	       ndpi_get_proto_name(ndpi_thread_info[0].workflow->ndpi_struct, i),
	       (long long unsigned int)cumulative_stats.protocol_counter[i],
	       (long long unsigned int)cumulative_stats.protocol_counter_bytes[i],
	       cumulative_stats.protocol_flows[i]);
      } else {
#ifdef HAVE_JSON_C
	if(json_fp) {
	  jObj = json_object_new_object();

	  json_object_object_add(jObj,"name",json_object_new_string(ndpi_get_proto_name(ndpi_thread_info[0].workflow->ndpi_struct, i)));
	  json_object_object_add(jObj,"breed",json_object_new_string(ndpi_get_proto_breed_name(ndpi_thread_info[0].workflow->ndpi_struct, breed)));
	  json_object_object_add(jObj,"packets",json_object_new_int64(cumulative_stats.protocol_counter[i]));
	  json_object_object_add(jObj,"bytes",json_object_new_int64(cumulative_stats.protocol_counter_bytes[i]));
	  json_object_object_add(jObj,"flows",json_object_new_int(cumulative_stats.protocol_flows[i]));

	  json_object_array_add(jArray_detProto,jObj);
	}
#endif
      }

      total_flow_bytes += cumulative_stats.protocol_counter_bytes[i];
    }
  }

  if((!json_flag) && (!quiet_mode)) {
    printf("\n\nProtocol statistics:\n");

    for(i=0; i < NUM_BREEDS; i++) {
      if(breed_stats[i] > 0) {
	printf("\t%-20s %13llu bytes\n",
	       ndpi_get_proto_breed_name(ndpi_thread_info[0].workflow->ndpi_struct, i),
	       breed_stats[i]);
      }
    }
  }

  // printf("\n\nTotal Flow Traffic: %llu (diff: %llu)\n", total_flow_bytes, cumulative_stats.total_ip_bytes-total_flow_bytes);

  if(verbose) {
    FILE *out = results_file ? results_file : stdout;

    if(!json_flag) fprintf(out, "\n");

    num_flows = 0;
    for(thread_id = 0; thread_id < num_threads; thread_id++) {
      for(i=0; i<NUM_ROOTS; i++)
        ndpi_twalk(ndpi_thread_info[thread_id].workflow->ndpi_flows_root[i], node_print_known_proto_walker, &thread_id);
    }

    for(thread_id = 0; thread_id < num_threads; thread_id++) {
      if(ndpi_thread_info[thread_id].workflow->stats.protocol_counter[0 /* 0 = Unknown */] > 0) {
        if(!json_flag) {
	  FILE *out = results_file ? results_file : stdout;

          fprintf(out, "\n\nUndetected flows:%s\n", undetected_flows_deleted ? " (expired flows are not listed below)" : "");
        }

	if(json_flag)
	  json_flag = 2;
        break;
      }
    }

    num_flows = 0;
    for(thread_id = 0; thread_id < num_threads; thread_id++) {
      if(ndpi_thread_info[thread_id].workflow->stats.protocol_counter[0] > 0) {
        for(i=0; i<NUM_ROOTS; i++)
	  ndpi_twalk(ndpi_thread_info[thread_id].workflow->ndpi_flows_root[i], node_print_unknown_proto_walker, &thread_id);
      }
    }
  }

  if(json_flag != 0) {
#ifdef HAVE_JSON_C
    json_object_object_add(jObj_main,"detected.protos",jArray_detProto);
    json_object_object_add(jObj_main,"known.flows",jArray_known_flows);

    if(json_object_array_length(jArray_unknown_flows) != 0)
      json_object_object_add(jObj_main,"unknown.flows",jArray_unknown_flows);

    fprintf(json_fp,"%s\n",json_object_to_json_string(jObj_main));
    fclose(json_fp);
#endif
  }
}

/* ***************************************************** */

static void breakPcapLoop(u_int16_t thread_id) {
  if(ndpi_thread_info[thread_id].workflow->pcap_handle != NULL) {
    pcap_breakloop(ndpi_thread_info[thread_id].workflow->pcap_handle);
  }
}

/* ***************************************************** */

// executed for each packet in the pcap file
void sigproc(int sig) {
  static int called = 0;
  int thread_id;

  if(called) return; else called = 1;
  shutdown_app = 1;

  for(thread_id=0; thread_id<num_threads; thread_id++)
    breakPcapLoop(thread_id);
}

/* ***************************************************** */

static int getNextPcapFileFromPlaylist(u_int16_t thread_id, char filename[], u_int32_t filename_len) {

  if(playlist_fp[thread_id] == NULL) {
    if((playlist_fp[thread_id] = fopen(_pcap_file[thread_id], "r")) == NULL)
      return -1;
  }

 next_line:
  if(fgets(filename, filename_len, playlist_fp[thread_id])) {
    int l = strlen(filename);
    if(filename[0] == '\0' || filename[0] == '#') goto next_line;
    if(filename[l-1] == '\n') filename[l-1] = '\0';
    return 0;
  } else {
    fclose(playlist_fp[thread_id]);
    playlist_fp[thread_id] = NULL;
    return -1;
  }
}

/* ***************************************************** */

static void configurePcapHandle(pcap_t * pcap_handle) {
  if(_bpf_filter != NULL) {
    struct bpf_program fcode;

    if(pcap_compile(pcap_handle, &fcode, _bpf_filter, 1, 0xFFFFFF00) < 0) {
      printf("pcap_compile error: '%s'\n", pcap_geterr(pcap_handle));
    } else {
      if(pcap_setfilter(pcap_handle, &fcode) < 0) {
	printf("pcap_setfilter error: '%s'\n", pcap_geterr(pcap_handle));
      } else
	printf("Successfully set BPF filter to '%s'\n", _bpf_filter);
    }
  }
}

/* ***************************************************** */

/* Always returns a valid pcap_t */
static pcap_t * openPcapFileOrDevice(u_int16_t thread_id, const u_char * pcap_file) {
  u_int snaplen = 1536;
  int promisc = 1;
  char pcap_error_buffer[PCAP_ERRBUF_SIZE];
  pcap_t * pcap_handle = NULL;

  /* trying to open a live interface */
  if((pcap_handle = pcap_open_live(pcap_file, snaplen, promisc, 500, pcap_error_buffer)) == NULL) {
    capture_for = capture_until = 0;

    live_capture = 0;
    num_threads = 1; /* Open pcap files in single threads mode */

    /* trying to open a pcap file */
    if((pcap_handle = pcap_open_offline(pcap_file, pcap_error_buffer)) == NULL) {
      char filename[256];

      /* trying to open a pcap playlist */
      if(getNextPcapFileFromPlaylist(thread_id, filename, sizeof(filename)) != 0 ||
	 (pcap_handle = pcap_open_offline(filename, pcap_error_buffer)) == NULL) {

        printf("ERROR: could not open pcap file or playlist: %s\n", pcap_error_buffer);
        exit(-1);
      } else {
        if((!json_flag) && (!quiet_mode)) printf("Reading packets from playlist %s...\n", pcap_file);
      }
    } else {
      if((!json_flag) && (!quiet_mode)) printf("Reading packets from pcap file %s...\n", pcap_file);
    }
  } else {
    live_capture = 1;

    if((!json_flag) && (!quiet_mode)) printf("Capturing live traffic from device %s...\n", pcap_file);
  }

  configurePcapHandle(pcap_handle);

  if(capture_for > 0) {
    if((!json_flag) && (!quiet_mode)) printf("Capturing traffic up to %u seconds\n", (unsigned int)capture_for);

#ifndef WIN32
    alarm(capture_for);
    signal(SIGALRM, sigproc);
#endif
  }
  
  return pcap_handle;
}

/* ***************************************************** */

static void pcap_packet_callback_checked(u_char *args,
				 const struct pcap_pkthdr *header,
				 const u_char *packet) {
  u_int16_t thread_id = *((u_int16_t*)args);
           
  /* allocate an exact size buffer to check overflows */
  uint8_t *packet_checked = malloc(header->caplen);
  memcpy(packet_checked, packet, header->caplen);
  ndpi_workflow_process_packet(ndpi_thread_info[thread_id].workflow, header, packet_checked);
  
  if((capture_until != 0) && (header->ts.tv_sec >= capture_until)) {
    if(ndpi_thread_info[thread_id].workflow->pcap_handle != NULL)
      pcap_breakloop(ndpi_thread_info[thread_id].workflow->pcap_handle);
    return;
  }
  
  /* Check if capture is live or not */
  if (!live_capture) {
    if (!pcap_start.tv_sec) pcap_start.tv_sec = header->ts.tv_sec, pcap_start.tv_usec = header->ts.tv_usec;
    pcap_end.tv_sec = header->ts.tv_sec, pcap_end.tv_usec = header->ts.tv_usec;
  }
  
  /* check for buffer changes */
  if(memcmp(packet, packet_checked, header->caplen) != 0)
    printf("INTERNAL ERROR: ingress packet was nodified by nDPI: this should not happen [thread_id=%u, packetId=%lu]\n",
	   thread_id, (unsigned long)ndpi_thread_info[thread_id].workflow->stats.raw_packet_count);
  free(packet_checked);
}

/* ******************************************************************** */

static void runPcapLoop(u_int16_t thread_id) {
  if((!shutdown_app) && (ndpi_thread_info[thread_id].workflow->pcap_handle != NULL))
    pcap_loop(ndpi_thread_info[thread_id].workflow->pcap_handle, -1, &pcap_packet_callback_checked, (u_char*)&thread_id);
}

/* ******************************************************************** */

void *processing_thread(void *_thread_id) {
  long thread_id = (long) _thread_id;
  char pcap_error_buffer[PCAP_ERRBUF_SIZE];

#if defined(linux) && defined(HAVE_PTHREAD_SETAFFINITY_NP)
  if(core_affinity[thread_id] >= 0) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_affinity[thread_id], &cpuset);

    if(pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0)
      fprintf(stderr, "Error while binding thread %ld to core %d\n", thread_id, core_affinity[thread_id]);
    else {
      if((!json_flag) && (!quiet_mode)) printf("Running thread %ld on core %d...\n", thread_id, core_affinity[thread_id]);
    }
  } else
#endif
    if((!json_flag) && (!quiet_mode)) printf("Running thread %ld...\n", thread_id);

 pcap_loop:
  runPcapLoop(thread_id);

  if(playlist_fp[thread_id] != NULL) { /* playlist: read next file */
    char filename[256];

    if(getNextPcapFileFromPlaylist(thread_id, filename, sizeof(filename)) == 0 &&
       (ndpi_thread_info[thread_id].workflow->pcap_handle = pcap_open_offline(filename, pcap_error_buffer)) != NULL) {
      configurePcapHandle(ndpi_thread_info[thread_id].workflow->pcap_handle);
      goto pcap_loop;
    }
  }
  
  return NULL;
}

/* ******************************************************************** */

void test_lib() {
  struct timeval begin, end;
  u_int64_t tot_usec;
  long thread_id;

#ifdef HAVE_JSON_C
  json_init();
#endif

  for(thread_id = 0; thread_id < num_threads; thread_id++) {
    pcap_t * cap = openPcapFileOrDevice(thread_id, _pcap_file[thread_id]);
    setupDetection(thread_id, cap);
  }

  gettimeofday(&begin, NULL);

  /* Running processing threads */
  for(thread_id = 0; thread_id < num_threads; thread_id++)
    pthread_create(&ndpi_thread_info[thread_id].pthread, NULL, processing_thread, (void *) thread_id);

  /* Waiting for completion */
  for(thread_id = 0; thread_id < num_threads; thread_id++)
    pthread_join(ndpi_thread_info[thread_id].pthread, NULL);

  gettimeofday(&end, NULL);
  tot_usec = end.tv_sec*1000000 + end.tv_usec - (begin.tv_sec*1000000 + begin.tv_usec);

  /* Printing cumulative results */
  printResults(tot_usec);

  for(thread_id = 0; thread_id < num_threads; thread_id++) {
    if(ndpi_thread_info[thread_id].workflow->pcap_handle != NULL) {
      pcap_close(ndpi_thread_info[thread_id].workflow->pcap_handle);
    }
    terminateDetection(thread_id);
  }
}

/* ***************************************************** */

int main(int argc, char **argv) {
  int i;

  memset(ndpi_thread_info, 0, sizeof(ndpi_thread_info));
  memset(&pcap_start, 0, sizeof(pcap_start));
  memset(&pcap_end, 0, sizeof(pcap_end));

  parseOptions(argc, argv);

  if((!json_flag) && (!quiet_mode)) {
    printf("\n-----------------------------------------------------------\n"
	   "* NOTE: This is demo app to show *some* nDPI features.\n"
	   "* In this demo we have implemented only some basic features\n"
	   "* just to show you what you can do with the library. Feel \n"
	   "* free to extend it and send us the patches for inclusion\n"
	   "------------------------------------------------------------\n\n");

    printf("Using nDPI (%s) [%d thread(s)]\n", ndpi_revision(), num_threads);
  }

  signal(SIGINT, sigproc);

  for(i=0; i<num_loops; i++)
    test_lib();

  if(results_path) free(results_path);
  if(results_file) fclose(results_file);

  return 0;
}

/* ****************************************************** */

#ifdef WIN32
#ifndef __GNUC__
#define EPOCHFILETIME (116444736000000000i64)
#else
#define EPOCHFILETIME (116444736000000000LL)
#endif

struct timezone {
  int tz_minuteswest; /* minutes W of Greenwich */
  int tz_dsttime;     /* type of dst correction */
};

/* ***************************************************** */

int gettimeofday(struct timeval *tv, struct timezone *tz) {
  FILETIME        ft;
  LARGE_INTEGER   li;
  __int64         t;
  static int      tzflag;

  if(tv) {
    GetSystemTimeAsFileTime(&ft);
    li.LowPart  = ft.dwLowDateTime;
    li.HighPart = ft.dwHighDateTime;
    t  = li.QuadPart;       /* In 100-nanosecond intervals */
    t -= EPOCHFILETIME;     /* Offset to the Epoch time */
    t /= 10;                /* In microseconds */
    tv->tv_sec  = (long)(t / 1000000);
    tv->tv_usec = (long)(t % 1000000);
  }

  if(tz) {
    if(!tzflag) {
      _tzset();
      tzflag++;
    }

    tz->tz_minuteswest = _timezone / 60;
    tz->tz_dsttime = _daylight;
  }

  return 0;
}
#endif /* WIN32 */

#include "aliveservice.h"

// #include "../misc/pcap_openvas.h" /* islocalhost() */
// #include "../misc/bpf_share.h"

#include <arpa/inet.h>
#include <errno.h>               /* for errno */
#include <gvm/base/networking.h> /* gvm_source_addr() */
#include <gvm/base/prefs.h>      /* for prefs_get() */
#include <gvm/util/kb.h>         /* kb_t ... */
#include <ifaddrs.h>             /* for getifaddrs() */
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/tcp.h>
#include <pcap.h>    /* pcap functions*/
#include <pthread.h> /* for threading */
#include <sys/param.h>
#include <sys/wait.h> /* for waitpid() */
#include <unistd.h>

/* for using int value in #defined string */
#define STR(X) #X
#define ASSTR(X) STR (X)
/* packets are sent to port 9910*/
#define FILTER_PORT 9910
#define FILTER_STR "ip and (icmp or dst port " ASSTR (FILTER_PORT) ")"

enum alive_detection
{
  ALIVE_DETECTION_FINISHED,
  ALIVE_DETECTION_SCANNING,
  ALIVE_DETECTION_OK,
  ALIVE_DETECTION_INIT,
  ALIVE_DETECTION_ERROR
};

/* global phandle for alive detection */
/* TODO: use static kb_t. connect to it on start and link_reset on finish */
pcap_t *handle;
static kb_t main_kb;
GHashTable *alivehosts;  /* (str, ?) */
GHashTable *targethosts; /* (str, gvm_host_t) */

/**
 * @return pcap_t handle or NULL on error
 */
pcap_t *
open_live (char *iface, char *filter)
{
  /* iface considerations:
   * pcap_open_live(iface, ...) sniffs on all interfaces(linux) if iface
   * argument is NULL pcap_lookupnet(iface, ...) is used to set ipv4 network
   * number and mask associated with iface pcap_compile(..., mask) netmask
   * specifies the IPv4 netmask of the network on which packets are being
   * captured; it is used only when checking for IPv4 broadcast addresses in the
   * filter program
   *
   *  If we are not checking for IPv4 broadcast addresses in the filter program
   * we do not need an iface (if we also want to listen on all interface) and we
   * do not need to call pcap_lookupnet
   */
  char errbuf[PCAP_ERRBUF_SIZE];
  pcap_t *ret;
  struct bpf_program filter_prog;

  /* iface, snapshot length of handle, promiscuous mode, packet buffer timeout
   * (ms), errbuff */
  ret = pcap_open_live (iface, 1500, 1, 0, errbuf);
  if (ret == NULL)
    {
      g_message ("%s", errbuf);
      return NULL;
    }

  /* needed for our usage of pcap_break_loop() */
  pcap_setnonblock (ret, 1, errbuf);

  if (pcap_compile (ret, &filter_prog, filter, 1, 0) < 0)
    {
      char *msg = pcap_geterr (ret);
      g_message ("pcap_compile : %s", msg);
      pcap_close (ret);
      return NULL;
    }

  if (pcap_setfilter (ret, &filter_prog) < 0)
    {
      char *msg = pcap_geterr (ret);
      g_message ("pcap_setfilter : %s", msg);
      pcap_close (ret);
      return NULL;
    }
  pcap_freecode (&filter_prog);

  return ret;
}

/**
 * @brief checks if addr is likely to be localhost
 *
 * @in: addr to check
 * @out: 1 if localhost, -1 on error
 * TODO: either make working for ipv6 too or write seperate method
 * ipv6islocalhost()
 */
int
islocalhost (struct in_addr *addr)
{
  int ret = 0;
  if (!addr)
    return -1;

  /* Addr is 0.0.0.0 */
  if (!addr->s_addr)
    return 1;
  /* Addr starts with 127. */
  if ((addr->s_addr & htonl (0xFF000000)) == htonl (0x7F000000))
    return 1;

  struct ifaddrs *ifaddr, *ifa;
  if (getifaddrs (&ifaddr) == -1)
    return -1;

  /* Search for the adequate interface/family. */
  for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
    {
      if (ifa->ifa_addr->sa_family == AF_INET)
        {
          struct in_addr *addr2 =
            &((struct sockaddr_in *) ifa->ifa_addr)->sin_addr;

          /* TODO: maybe compare strings instead of addresses */
          if ((int) (*addr).s_addr == (int) (*addr2).s_addr)
            ret = 1;
        }
      /* TODO: ipv6
       * else if (ifa->ifa_addr->sa_family == AF_INET6)
       */
    }
  freeifaddrs (ifaddr);
  return ret;
}

/**
 * @brief get new item from queue
 *
 * @in: flag which is set to status of scan process
 * @out: host_string or NULL if no item present or finish signal
 */
static char *
get_alive_host_str (int *flag)
{
  char *host = NULL;
  /* handle race condition. main_kb may not yet be initialized */
  /* TODO: find better solution  */
  if (!main_kb)
    {
      *flag = ALIVE_DETECTION_INIT;
      return NULL;
    }

  host = kb_item_pop_str (main_kb, ("alive_detection"));
  /* 3 if item is not found return NULL and set flag to ALIVE_DETECTION_SCANNING
   */
  if (host == NULL)
    {
      *flag = ALIVE_DETECTION_SCANNING;
      return NULL;
    }
  /* 3 if item is 'finish' return NULL and set flag to ALIVE_DETECTION_FINISHED
   */
  else if (host != NULL && (g_strcmp0 (host, "finish") == 0))
    {
      *flag = ALIVE_DETECTION_FINISHED;
      return NULL;
    }
  /* 3 if item is host_str return host_str and set flag to ALIVE_DETECTION_OK */
  else
    {
      *flag = ALIVE_DETECTION_OK;
      return host;
    }
}

/**
 * @brief get new host from queue and put it into an gvm_host_t struct
 *
 * @in:  timeout for waiting for new alive host. If timout <= 0 we wait
 * 'indefinetly'(INT_MAX seconds)
 * @out: host structure from Queue
 *
 */
gvm_host_t *
get_host_from_queue (int timeout)
{
  /* default timeout is indef. (until alive detection process is finished) */
  if (timeout <= 0)
    timeout = INT_MAX;

  char *host_str = NULL;
  int alive_detection_flag = 0;
  gvm_host_t *host = NULL;

  g_message ("%s: get new host from Queue", __func__);
  host_str = get_alive_host_str (
    &alive_detection_flag); /* get host string from Queue or NULL*/

  if (host_str)
    host = gvm_host_from_str (host_str);
  while (!host && (alive_detection_flag != ALIVE_DETECTION_FINISHED)
         && timeout--)
    {
      sleep (1);
      host_str = get_alive_host_str (&alive_detection_flag);
      host = gvm_host_from_str (host_str);
    }

  if (alive_detection_flag == ALIVE_DETECTION_FINISHED)
    {
      host = NULL;
    }
  g_free (host_str);
  return host;
}

/* TODO: */
struct pseudohdr
{
  struct in_addr saddr;
  struct in_addr daddr;
  u_char zero;
  u_char protocol;
  u_short length;
  struct tcphdr tcpheader;
};

/*
 * Checksum routine for Internet Protocol family headers (C Version)
 * From ping examples in W.Richard Stevens "UNIX NETWORK PROGRAMMING" book.
 * TODO:
 */
static int np_in_cksum (p, n) u_short *p;
int n;
{
  register u_short answer;
  register long sum = 0;
  u_short odd_byte = 0;

  while (n > 1)
    {
      sum += *p++;
      n -= 2;
    }

  /* mop up an odd byte, if necessary */
  if (n == 1)
    {
      *(u_char *) (&odd_byte) = *(u_char *) p;
      sum += odd_byte;
    }

  sum = (sum >> 16) + (sum & 0xffff); /* add hi 16 to low 16 */
  sum += (sum >> 16);                 /* add carry */
  answer = (int) ~sum;                /* ones-complement, truncate */
  return (answer);
}

// // copy of function in attack.c
// static void
// fork_sleep (int n)
// {
//   time_t then, now;

//   now = then = time (NULL);
//   while (now - then < n)
//     {
//       waitpid (-1, NULL, WNOHANG);
//       usleep (10000);
//       now = time (NULL);
//     }
// }

void
got_packet (__attribute__ ((unused)) u_char *args,
            __attribute__ ((unused)) const struct pcap_pkthdr *header,
            const u_char *packet)
{
  struct in_addr sniffed_addr;
  /* was +26 originally but was off by 2 somehow */
  memcpy (&sniffed_addr.s_addr, packet + 26 + 2, 4);
  gchar *addr_str = inet_ntoa (sniffed_addr);
  /* Do not put already found host on Queue and only put hosts on Queue we are
   * seaching for. */
  if (g_hash_table_add (alivehosts, g_strdup (addr_str))
      && g_hash_table_contains (targethosts, addr_str) == TRUE)
    {
      g_message ("%s: Thread sniffed unique address to put on queue: %s",
                 __func__, addr_str);
      kb_item_push_str (main_kb, "alive_detection", addr_str);
    }
}

static void *
sniffer_thread (__attribute__ ((unused)) void *vargp)
{
  int ret;
  g_message ("%s: start sniffing", __func__);

  /* reads packets until error or pcap_breakloop() */
  if ((ret = pcap_loop (handle, -1, got_packet, NULL)) == PCAP_ERROR)
    g_warning ("%s: pcap_loop error %s", __func__, pcap_geterr (handle));
  else if (ret == 0)
    g_warning ("%s: count of packets is exhausted", __func__);
  else if (ret == PCAP_ERROR_BREAK)
    g_message ("%s: Loop was succesfully broken after call to pcap_breakloop",
               __func__);

  pthread_exit (0);
}

static void
set_src_addr (struct in_addr *src)
{
  /* check if src addr already set. get host addr if not already set. */
  gvm_source_addr (src);
  if (src->s_addr)
    {
      g_debug ("%s: We use global_source_addr as src because it was "
               "already set by apply_source_iface_preference",
               __func__);
    }
  else
    {
      /* TODO: put in seperate function */
      struct ifaddrs *ifaddr, *ifa;
      if (getifaddrs (&ifaddr) == -1)
        return; // better return value or message
      for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
        {
          if (!ifa->ifa_addr)
            {
              continue;
            }
          if (ifa->ifa_addr->sa_family == AF_INET)
            {
              struct in_addr *addr =
                &((struct sockaddr_in *) ifa->ifa_addr)->sin_addr;

              memcpy (src, addr, sizeof (struct in_addr));
            }
          /* ipv6 */
          /* else if (ifa->ifa_addr->sa_family == AF_INET6){} */
        }
    }
}

__attribute__ ((unused)) static void
send_icmps (__attribute__ ((unused)) gpointer key, gpointer value,
            gpointer user_data)
{
  struct sockaddr_in soca;
  int soc = *((gint *) user_data);

  u_char packet[sizeof (struct ip) + sizeof (struct icmp)];
  struct ip *ip = (struct ip *) packet;
  struct icmp *icmp = (struct icmp *) (packet + sizeof (struct ip));

  struct in_addr inaddr; /* ip dst */
  struct in_addr src;    /* ip src */

  struct in6_addr dst_p;
  struct in6_addr *dst = &dst_p;

  /* get dst address */
  if (gvm_host_get_addr6 ((gvm_host_t *) value, dst) < 0)
    g_message ("%s: Some error while gvm_host_get_addr6", __func__);
  if (dst == NULL || (IN6_IS_ADDR_V4MAPPED (dst) != 1))
    {
      g_debug ("%s: is ipv6 addr", __func__);
      /* TODO: ipv6 */
      return;
    }
  inaddr.s_addr = dst->s6_addr32[3];

  /* get src address */
  if (islocalhost (&inaddr) > 0)
    src.s_addr = dst->s6_addr32[3];
  else
    set_src_addr (&src);

  /* construct packet */
  bzero (packet, sizeof (packet));

  /* IP */
  ip->ip_hl = 5;
  ip->ip_off = htons (0);
  ip->ip_v = 4;
  ip->ip_len = htons (40); // total length, maybe more
  ip->ip_tos = 0;
  ip->ip_p = IPPROTO_ICMP;
  ip->ip_id = rand ();
  ip->ip_ttl = 0x40;
  ip->ip_src = src;
  ip->ip_dst = inaddr;
  ip->ip_sum = 0;
  ip->ip_sum = np_in_cksum ((u_short *) ip, 20);

  /* icmp */
  icmp->icmp_type = ICMP_ECHO;
  icmp->icmp_code = 0;
  icmp->icmp_id = rand (); // 123; AA
  icmp->icmp_seq = 0;      // AA
  icmp->icmp_cksum = 0;
  icmp->icmp_cksum =
    np_in_cksum ((u_short *) icmp, sizeof (packet) - sizeof (struct icmp));

  /* send packet */
  bzero (&soca, sizeof (soca));
  soca.sin_family = AF_INET;
  soca.sin_addr = ip->ip_dst;
  if (sendto (soc, (const void *) ip, 40, 0, (struct sockaddr *) &soca,
              sizeof (soca))
      < 0)
    g_warning ("sendto: %s", strerror (errno));
}

static int
get_socket (void)
{
  int soc;
  int opt = 1;
  soc = socket (AF_INET, SOCK_RAW, IPPROTO_RAW);
  if (soc < 0)
    {
      g_critical (
        "%s: failed to set socket options on alive detection socket: %s",
        __func__, strerror (errno));
      return -1;
    }
  if (setsockopt (soc, IPPROTO_IP, IP_HDRINCL, (char *) &opt, sizeof (opt)) < 0)
    {
      g_critical (
        "%s: failed to set socket options on alive detection socket: %s",
        __func__, strerror (errno));
      return -1;
    }
  return soc;
}

/**
 * @brief Delete alive hosts from targethosts
 *
 * @param targethosts   target_hosts hashtable
 *
 */
void
exclude (gpointer key, __attribute__ ((unused)) gpointer value,
         gpointer targethosts)
{
  /* delte key from targethost*/
  g_hash_table_remove (targethosts, (gchar *) key);
}

void
print_host_str (gpointer key, __attribute__ ((unused)) gpointer value,
                __attribute__ ((unused)) gpointer user_data)
{
  g_message ("host_str: %s", (gchar *) key);
}

void
tcp_syns (__attribute__ ((unused)) gpointer key, gpointer value,
          gpointer user_data)
{
  struct sockaddr_in soca;
  int soc = *((gint *) user_data);

  u_char packet[sizeof (struct ip) + sizeof (struct tcphdr)];
  struct ip *ip = (struct ip *) packet;
  struct tcphdr *tcp = (struct tcphdr *) (packet + sizeof (struct ip));

  struct in_addr inaddr; /* ip dst */
  struct in_addr src;    /* ip src */

  struct in6_addr dst_p;
  struct in6_addr *dst = &dst_p;

  int port = 0;
  int ports[] = {139, 135, 445,  80,    22,   515, 23,  21,  6000, 1025,
                 25,  111, 1028, 9100,  1029, 79,  497, 548, 5000, 1917,
                 53,  161, 9001, 65535, 443,  113, 993, 8080};

  /* get dst address */
  if (gvm_host_get_addr6 ((gvm_host_t *) value, dst) < 0)
    g_message ("%s: Some error while gvm_host_get_addr6", __func__);
  if (dst == NULL || (IN6_IS_ADDR_V4MAPPED (dst) != 1))
    {
      g_debug ("%s: is ipv6 addr", __func__);
      /* TODO: ipv6 */
      return;
    }
  inaddr.s_addr = dst->s6_addr32[3];

  /* get src address */
  if (islocalhost (&inaddr) > 0)
    src.s_addr = dst->s6_addr32[3];
  else
    set_src_addr (&src);

  /* for ports in portrange send packets */
  for (long unsigned int i = 0; i < sizeof (ports) / sizeof (int); i++)
    {
      bzero (packet, sizeof (packet));
      /* IP */
      ip->ip_hl = 5;
      ip->ip_off = htons (0);
      ip->ip_v = 4;
      ip->ip_len = htons (40);
      ip->ip_tos = 0;
      ip->ip_p = IPPROTO_TCP;
      ip->ip_id = rand ();
      ip->ip_ttl = 0x40;
      ip->ip_src = src;
      ip->ip_dst = inaddr;
      ip->ip_sum = 0;
      ip->ip_sum = np_in_cksum ((u_short *) ip, 20);

      /* TCP */
      tcp->th_sport = htons (FILTER_PORT);
      tcp->th_flags = TH_SYN;
      tcp->th_dport = port ? htons (port) : htons (ports[i]);
      tcp->th_seq = rand ();
      tcp->th_ack = 0;
      tcp->th_x2 = 0;
      tcp->th_off = 5;
      tcp->th_win = 2048;
      tcp->th_urp = 0;
      tcp->th_sum = 0;

      /* CKsum */
      {
        struct in_addr source, dest;
        struct pseudohdr pseudoheader;
        source.s_addr = ip->ip_src.s_addr;
        dest.s_addr = ip->ip_dst.s_addr;

        bzero (&pseudoheader,
               12 + sizeof (struct tcphdr)); // bzero is deprecated. use
                                             // memset(3) instead
        pseudoheader.saddr.s_addr = source.s_addr;
        pseudoheader.daddr.s_addr = dest.s_addr;

        pseudoheader.protocol = 6;
        pseudoheader.length = htons (sizeof (struct tcphdr));
        bcopy ((char *) tcp,
               (char *) &pseudoheader.tcpheader, // bcopy is deprecated. use
                                                 // memcpy(3) or memmove(3) ?
               sizeof (struct tcphdr));
        tcp->th_sum = np_in_cksum ((unsigned short *) &pseudoheader,
                                   12 + sizeof (struct tcphdr));
      }

      bzero (&soca, sizeof (soca));
      soca.sin_family = AF_INET;
      soca.sin_addr = ip->ip_dst;
      if (sendto (soc, (const void *) ip, 40, 0, (struct sockaddr *) &soca,
                  sizeof (soca))
          < 0)
        g_warning ("sendto: %s", strerror (errno));
    }
}

static int
ping (void)
{
  pthread_t tid; /* thread id */
  int soc;       /* socket */

  handle = open_live (NULL, FILTER_STR);
  soc = get_socket ();

  /* ICMP */
  pthread_create (&tid, NULL, sniffer_thread, NULL);

  g_hash_table_foreach (targethosts, send_icmps, &soc);

  /* wait for replies and break loop */
  sleep (3);
  pcap_breakloop (handle);
  g_message ("%s: break_loop", __func__);

  /* join thread*/
  if (pthread_join (tid, NULL) != 0)
    g_warning ("%s: got error from pthread_join", __func__);
  g_message ("%s: join thread", __func__);

  /* exclude alivehosts form targethosts so we dont test them again */
  g_hash_table_foreach (alivehosts, exclude, targethosts);

  /* TCP SYN */
  pthread_create (&tid, NULL, sniffer_thread, NULL);
  g_hash_table_foreach (targethosts, tcp_syns, &soc);

  /* wait for replies and break loop */
  sleep (3);
  pcap_breakloop (handle);
  g_message ("%s: break_loop", __func__);

  /* join thread*/
  if (pthread_join (tid, NULL) != 0)
    g_warning ("%s: got error from pthread_join", __func__);
  g_message ("%s: join thread", __func__);

  /* exclude alivehosts form targethosts so we dont test them again */
  g_hash_table_foreach (alivehosts, exclude, targethosts);

  /* close handle */
  if (handle != NULL)
    {
      g_message ("%s: close pcap handle", __func__);
      pcap_close (handle);
    }

  /* close socket */
  close (soc);
  g_message ("%s: close socket ", __func__);

  return 0;
}

/**
 * @brief start the tcp_syn scan of all specified hosts in gvm_hosts_t list.
 * Finish signal is put on Queue if pinger returned.
 *
 * @in: gvm_hosts_t structure
 */
void *
start_alive_detection (void *args)
{
  gvm_hosts_t *hosts = (gvm_hosts_t *) args;
  int err;
  int scandb_id = atoi (prefs_get ("ov_maindbid"));
  /* This kb_t is only used once every alive detection process */
  main_kb = kb_direct_conn (prefs_get ("db_address"), scandb_id);

  targethosts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  alivehosts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  /* put all hosts we want to check in hashtable */
  gvm_host_t *host;
  for (host = gvm_hosts_next (hosts); host; host = gvm_hosts_next (hosts))
    {
      g_hash_table_insert (targethosts, gvm_host_value_str (host), host);
    }
  /* reset iter */
  hosts->current = 0;

  g_message ("%s: alive detection process started", __func__);
  /* blocks until detection process is finished */
  err = ping ();
  if (err < 0)
    g_warning ("%s: pinger returned some error code", __func__);

  /* put finish signal on Q if all packets were send and we waited long enough
   * for packets to arrive */
  kb_item_push_str (main_kb, "alive_detection", "finish");
  kb_lnk_reset (main_kb);

  g_message ("%s: alive detection process finished. finish signal put on Q.",
             __func__);

  // g_message ("%s sleep.", __func__);
  // sleep(50); // debugging process termination
  // g_message ("%s: slept.", __func__);
  g_hash_table_destroy (targethosts);
  g_hash_table_destroy (alivehosts);

  pthread_exit (0);
}
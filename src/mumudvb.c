/* 
mumudvb - UDP-ize a DVB transport stream.
(C) Dave Chapman <dave@dchapman.com> 2001, 2002.
Modified By Brice DUBOST
 * 
The latest version can be found at http://www.crans.org

Copyright notice:

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
    
*/
  /* ATTENTION
     Version modifiee par braice (mumudvb@braice.net) pour le Crans (www.crans.org)
     me demander pour toute documentation)
   */


#define _GNU_SOURCE		// pour utiliser le program_invocation_short_name (extension gnu)

// Linux includes:
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/time.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <resolv.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <values.h>
#include <string.h>
#include <syslog.h>
#include <getopt.h>
#include <errno.h>		// pour utiliser le program_invocation_short_name (extension gnu)

#include "mumudvb.h"
#include "tune.h"
#include "udp.h"
#include "dvb.h"
#include "errors.h"

#define VERSION "1.2"


/* Signal handling code shamelessly copied from VDR by Klaus Schmidinger 
   - see http://www.cadsoft.de/people/kls/vdr/index.htm */

// global variable user by SignalHandler
long now;
long time_no_diff;
long real_start_time;
int aff_force_signal = 0;
int no_daemon = 0;
int nb_flux;
int chaines_diffuses[MAX_CHAINES];
int chaines_diffuses_old[MAX_CHAINES];
char noms[MAX_CHAINES][MAX_LEN_NOM];
int carte_accordee = 0;
int timeout_accord = ALARM_TIME_TIMEOUT;
int timeout_no_diff = ALARM_TIME_TIMEOUT_NO_DIFF;
int Interrupted = 0;
char nom_fich_chaines_diff[256];
int card = 0;


// file descriptors
fds_t fds; // defined in dvb.h


// prototypes
static void SignalHandler (int signum);
void gen_chaines_diff (int no_daemon, int *chaines_diffuses);


void
usage (char *name)
{
  fprintf (stderr, "Usage: %s [options] \n"
	   "-c, --config : Config file\n"
	   "-s, --signal : Display signal power\n"
	   "-d, --debug  : Don't deamonize\n"
	   "-h, --help   : Help\n"
	   "\n"
	   "%s Version "
	   VERSION
	   "\n"
	   "Based on dvbstream 0.6 by (C) Dave Chapman 2001-2004\n"
	   "Released under the GPL.\n"
	   "Latest version available from http://mumudvb.braice.net/\n"
	   "Modified for the cr@ns (www.crans.org)\n"
	   "by Brice DUBOST (mumudvb@braice.net)\n", name, name);
}


int
main (int argc, char **argv)
{
  int k;
  int buf_pos;
  unsigned char buf[MAX_CHAINES][MAX_UDP_SIZE];
  struct pollfd pfds[2];	//  DVR device


  // r�ception
  unsigned long freq = 0;
  unsigned long srate = 0;
  char pol = 0;

  // DVB reception and sort
  int pid;			// pid of the current mpeg2 packet
  int bytes_read;		// number of bytes actually read
  unsigned char temp_buf[MAX_UDP_SIZE];
  int nb_bytes[MAX_CHAINES];

  struct timeval tv;

  // fichers
  char *conf_filename = NULL;
  FILE *conf_file;
  char nom_fich_pid[256];
  // lock et chaines diffus�es
  FILE *chaines_diff;
  FILE *pidfile;

  // configuration file parsing
  int curr_channel = 0;
  int curr_pid = 0;
  int port_ok = 0;
  int ip_ok = 0;
  char ligne_courante[CONF_LINELEN];
  char *sous_chaine;
  char delimiteurs[] = " =";


  fe_spectral_inversion_t specInv = INVERSION_AUTO;
  int tone = -1;
  //parametres TNT a changer
  fe_modulation_t modulation = CONSTELLATION_DEFAULT;
  fe_transmit_mode_t TransmissionMode = TRANSMISSION_MODE_DEFAULT;
  fe_bandwidth_t bandWidth = BANDWIDTH_DEFAULT;
  fe_guard_interval_t guardInterval = GUARD_INTERVAL_DEFAULT;
  fe_code_rate_t HP_CodeRate = HP_CODERATE_DEFAULT, LP_CodeRate =
    LP_CODERATE_DEFAULT;

  fe_hierarchy_t hier = HIERARCHY_DEFAULT;
  unsigned char diseqc = 0;
  unsigned char hi_mappids[8192];
  unsigned char lo_mappids[8192];
  int pids[MAX_CHAINES][MAX_PIDS_PAR_CHAINE];
  int num_pids[MAX_CHAINES];
  int alarm_count = 0;
  int count_non_transmis = 0;
  int tune_retval=0;

  const char short_options[] = "c:sdh";
  const struct option long_options[] = {
    {"config", required_argument, NULL, 'c'},
    {"signal", no_argument, NULL, 's'},
    {"debug", no_argument, NULL, 'd'},
    {"help", no_argument, NULL, 'h'},
    {0, 0, 0, 0}
  };
  int c, option_index = 0;
  // Initialise PID map
  for (k = 0; k < 8192; k++)
    {
      hi_mappids[k] = (k >> 8);
      lo_mappids[k] = (k & 0xff);
    }

  if (argc == 1)
    {
      usage (argv[0]);
      exit(ERROR_ARGS);
    }
  while (1)
    {
      c = getopt_long (argc, argv, short_options,
		       long_options, &option_index);

      if (c == -1)
	{
	  break;
	}
      switch (c)
	{
	case 'c':
	  conf_filename = (char *) malloc (strlen (optarg) + 1);
	  if (!conf_filename)
	    {
	      fprintf(stderr, "malloc() failed: %s\n", strerror(errno));
	      exit(errno);
	    }
	  strncpy (conf_filename, optarg, strlen (optarg) + 1);
	  break;
	case 's':
	  aff_force_signal = 1;
	  break;
	case 'd':
	  no_daemon = 1;
	  break;
	case 'h':
	  usage (program_invocation_short_name);
	  exit(ERROR_ARGS);
	  break;
	}
    }
  if (optind < argc)
    {
      usage (program_invocation_short_name);
      exit(ERROR_ARGS);
    }

  // DO NOT REMOVE
  if(!no_daemon)
    {
      daemon(42,0);
    }

  if (!no_daemon)
    openlog ("MUMUDVB", LOG_PID, 0);

  // fichier de conf
  conf_file = fopen (conf_filename, "r");
  if (conf_file == NULL)
    {
      if (!no_daemon)
	syslog (LOG_USER, "%s: %s\n",
		conf_filename, strerror (errno));
      else
	fprintf (stderr,
		 "%s: %s\n",
		 conf_filename, strerror (errno));
      exit(ERROR_CONF_FILE);
    }

  memset (pids, 0, sizeof (pids));



  // on scanne le fichier de conf
  // la syntaxe de celui ci est dans syntaxe_conf.txt
  while (fgets (ligne_courante, CONF_LINELEN, conf_file)
	 && strlen (ligne_courante) > 1)
    {
      sous_chaine = strtok (ligne_courante, delimiteurs);
      //commentaires
      if (sous_chaine[0] == '#')
	continue; 
      
      if (!strcmp (sous_chaine, "timeout_accord"))
	{
	  sous_chaine = strtok (NULL, delimiteurs);	// on extrait la sous chaine
	  timeout_accord = atoi (sous_chaine);
	}
      else if (!strcmp (sous_chaine, "timeout_no_diff"))
	{
	  sous_chaine = strtok (NULL, delimiteurs);
	  timeout_no_diff= atoi (sous_chaine);
	}
      else if (!strcmp (sous_chaine, "freq"))
	{
	  sous_chaine = strtok (NULL, delimiteurs);
	  freq = atol (sous_chaine);
	  freq *= 1000UL;
	}
      else if (!strcmp (sous_chaine, "pol"))
	{
	  sous_chaine = strtok (NULL, delimiteurs);
	  if (tolower (sous_chaine[0]) == 'v')
	    {
	      pol = 'V';
	    }
	  else if (tolower (sous_chaine[0]) == 'h')
	    {
	      pol = 'H';
	    }
	  else
	    {
	      if (!no_daemon)
		syslog (LOG_USER,
			"Config issue : %s polarisation\n",
			conf_filename);
	      else
		fprintf (stderr,
			 "Config issue : %s polarisation\n",
			 conf_filename);
	      exit(ERROR_CONF);
	    }
	}
      else if (!strcmp (sous_chaine, "srate"))
	{
	  sous_chaine = strtok (NULL, delimiteurs);
	  srate = atol (sous_chaine);
	  srate *= 1000UL;
	}
      else if (!strcmp (sous_chaine, "card"))
	{
	  sous_chaine = strtok (NULL, delimiteurs);
	  card = atoi (sous_chaine);
	  if (card > 5)
	    {
	      if (!no_daemon)
		syslog (LOG_USER,
			"Six cards MAX\n");
	      else
		fprintf (stderr,
			"Six cards MAX\n");
	      exit(ERROR_CONF);
	    }
	}
      else if (!strcmp (sous_chaine, "ip"))
	{
	  sous_chaine = strtok (NULL, delimiteurs);
	  sscanf (sous_chaine, "%s\n", ipOut[curr_channel]);
	  ip_ok = 1;
	}
      else if (!strcmp (sous_chaine, "port"))
	{
	  sous_chaine = strtok (NULL, delimiteurs);
	  portOut[curr_channel] = atoi (sous_chaine);
	  port_ok = 1;
	}
      else if (!strcmp (sous_chaine, "pids"))
	{
	  if (port_ok == 0 || ip_ok == 0)
	    {
	      if (!no_daemon)
		syslog (LOG_USER,
			"You must precise ip and port before PIDs\n");
	      else
		fprintf (stderr,
			"You must precise ip and port before PIDs\n");
	      exit(ERROR_CONF);
	    }
	  while ((sous_chaine = strtok (NULL, delimiteurs)) != NULL)
	    {
	      pids[curr_channel][curr_pid] = atoi (sous_chaine);
	      // on verifie la validit� du pid donn�
	      if (pids[curr_channel][curr_pid] < 10 || pids[curr_channel][curr_pid] > 8191)
		{
		  if (!no_daemon)
		    syslog (LOG_USER,
			    "Config issue : %s in pids, given pid : %d\n",
			    conf_filename, pids[curr_channel][curr_pid]);
		  else
		    fprintf (stderr,
			    "Config issue : %s in pids, given pid : %d\n",
			     conf_filename, pids[curr_channel][curr_pid]);
		  exit(ERROR_CONF);
		}
	      curr_pid++;
	      if (curr_pid >= MAX_PIDS_PAR_CHAINE)
		{
		  if (!no_daemon)
		    syslog (LOG_USER,
			    "Too many pids : %d channel : %d\n",
			    curr_pid, curr_channel);
		  else
		    fprintf (stderr,
			    "Too many pids : %d channel : %d\n",
			     curr_pid, curr_channel);
		  exit(ERROR_CONF);
		}
	    }
	  num_pids[curr_channel] = curr_pid;
	  curr_pid = 0;
	  curr_channel++;
	  port_ok = 0;
	  ip_ok = 0;
	}
      else if (!strcmp (sous_chaine, "name"))
	{
	  // changement de m�thode d'extraction pour garder les espaces
	  sous_chaine = strtok (NULL, "=");
	  if (!(strlen (sous_chaine) >= MAX_LEN_NOM - 1))
	    strcpy(noms[curr_channel],strtok(sous_chaine,"\n"));	
	  else
	    fprintf (stderr, "Channel name too long\n");
	}
      else if (!strcmp (sous_chaine, "qam"))
	{
	  // TNT
	  sous_chaine = strtok (NULL, delimiteurs);
	  sscanf (sous_chaine, "%s\n", sous_chaine);
	  if (!strcmp (sous_chaine, "qpsk"))
	    modulation=QPSK;
	  else if (!strcmp (sous_chaine, "16"))
	    modulation=QAM_16;
	  else if (!strcmp (sous_chaine, "32"))
	    modulation=QAM_32;
	  else if (!strcmp (sous_chaine, "64"))
	    modulation=QAM_64;
	  else if (!strcmp (sous_chaine, "128"))
	    modulation=QAM_128;
	  else if (!strcmp (sous_chaine, "256"))
	    modulation=QAM_256;
	  else if (!strcmp (sous_chaine, "auto"))
	    modulation=QAM_AUTO;
	  else
	    {
	      if (!no_daemon)
		syslog (LOG_USER,
			"Config issue : QAM\n");
	      else
		fprintf (stderr,
			"Config issue : QAM\n");
	      exit(ERROR_CONF);
	    }
	}
      else if (!strcmp (sous_chaine, "trans_mode"))
	{
	  // TNT
	  sous_chaine = strtok (NULL, delimiteurs);
	  sscanf (sous_chaine, "%s\n", sous_chaine);
	  if (!strcmp (sous_chaine, "2k"))
	    TransmissionMode=TRANSMISSION_MODE_2K;
	  else if (!strcmp (sous_chaine, "8k"))
	    TransmissionMode=TRANSMISSION_MODE_8K;
	  else if (!strcmp (sous_chaine, "auto"))
	    TransmissionMode=TRANSMISSION_MODE_AUTO;
	  else
	    {
	      if (!no_daemon)
		syslog (LOG_USER,
			"Config issue : trans_mode\n");
	      else
		fprintf (stderr,
			"Config issue : trans_mode\n");
	      exit(ERROR_CONF);
	    }
	}
      else if (!strcmp (sous_chaine, "bandwidth"))
	{
	  // TNT
	  sous_chaine = strtok (NULL, delimiteurs);
	  sscanf (sous_chaine, "%s\n", sous_chaine);
	  if (!strcmp (sous_chaine, "8MHz"))
	    bandWidth=BANDWIDTH_8_MHZ;
	  else if (!strcmp (sous_chaine, "7MHz"))
	    bandWidth=BANDWIDTH_7_MHZ;
	  else if (!strcmp (sous_chaine, "6MHz"))
	    bandWidth=BANDWIDTH_6_MHZ;
	  else if (!strcmp (sous_chaine, "auto"))
	    bandWidth=BANDWIDTH_AUTO;
	  else
	    {
	      if (!no_daemon)
		syslog (LOG_USER,
			"Config issue : bandwidth\n");
	      else
		fprintf (stderr,
			"Config issue : bandwidth\n");
	      exit(ERROR_CONF);
	    }
	}
      else if (!strcmp (sous_chaine, "guardinterval"))
	{
	  // TNT
	  sous_chaine = strtok (NULL, delimiteurs);
	  sscanf (sous_chaine, "%s\n", sous_chaine);
	  if (!strcmp (sous_chaine, "1/32"))
	    guardInterval=GUARD_INTERVAL_1_32;
	  else if (!strcmp (sous_chaine, "1/16"))
	    guardInterval=GUARD_INTERVAL_1_16;
	  else if (!strcmp (sous_chaine, "1/8"))
	    guardInterval=GUARD_INTERVAL_1_8;
	  else if (!strcmp (sous_chaine, "1/4"))
	    guardInterval=GUARD_INTERVAL_1_4;
	  else if (!strcmp (sous_chaine, "auto"))
	    guardInterval=GUARD_INTERVAL_AUTO;
	  else
	    {
	      if (!no_daemon)
		syslog (LOG_USER,
			"Config issue : guardinterval\n");
	      else
		fprintf (stderr,
			 "Config issue : guardinterval\n");
	      exit(ERROR_CONF);
	    }
	}
      else if (!strcmp (sous_chaine, "coderate"))
	{
	  // TNT
	  sous_chaine = strtok (NULL, delimiteurs);
	  sscanf (sous_chaine, "%s\n", sous_chaine);
	  if (!strcmp (sous_chaine, "none"))
	    HP_CodeRate=FEC_NONE;
	  else if (!strcmp (sous_chaine, "1/2"))
	    HP_CodeRate=FEC_1_2;
	  else if (!strcmp (sous_chaine, "2/3"))
	    HP_CodeRate=FEC_2_3;
	  else if (!strcmp (sous_chaine, "3/4"))
	    HP_CodeRate=FEC_3_4;
	  else if (!strcmp (sous_chaine, "4/5"))
	    HP_CodeRate=FEC_4_5;
	  else if (!strcmp (sous_chaine, "5/6"))
	    HP_CodeRate=FEC_5_6;
	  else if (!strcmp (sous_chaine, "6/7"))
	    HP_CodeRate=FEC_6_7;
	  else if (!strcmp (sous_chaine, "7/8"))
	    HP_CodeRate=FEC_7_8;
	  else if (!strcmp (sous_chaine, "8/9"))
	    HP_CodeRate=FEC_8_9;
	  else if (!strcmp (sous_chaine, "auto"))
	    HP_CodeRate=FEC_AUTO;
	  else
	    {
	      if (!no_daemon)
		syslog (LOG_USER,
			"Config issue : coderate\n");
	      else
		fprintf (stderr,
			"Config issue : coderate\n");
	      exit(ERROR_CONF);
	    }
	  LP_CodeRate=HP_CodeRate; // je ne sais pas tres bien ce que cela change mais je les met egales
	}
      else
	{
	  /*probleme */
	  continue;
	}
    }
  nb_flux = curr_channel;
  if (curr_channel > MAX_CHAINES)
    {
      if (!no_daemon)
	syslog (LOG_USER, "Too many channels : %d limit : %d\n",
		curr_channel, MAX_CHAINES);
      else
	fprintf (stderr, "Too many channels : %d limit : %d\n",
		 curr_channel, MAX_CHAINES);
      exit(ERROR_TOO_CHANNELS);
    }

  // on le vide, pr�caution
  sprintf (nom_fich_chaines_diff, "/var/run/mumudvb/chaines_diffusees_carte%d",
	   card);
  chaines_diff = fopen (nom_fich_chaines_diff, "w");
  if (chaines_diff == NULL)
    {
      if (!no_daemon)
	syslog (LOG_USER,
		"%s: %s\n",
		nom_fich_chaines_diff, strerror (errno));
      else
	fprintf (stderr,
		 "%s: %s\n",
		 nom_fich_chaines_diff, strerror (errno));
      exit(ERROR_CREATE_FILE);
    }

  fclose (chaines_diff);

  if (!no_daemon)
    syslog (LOG_USER, "Diffusion. Freq %lu pol %c srate %lu\n",
	    freq, pol, srate);

  // alarme pour le d�lai d'accord
  if (signal (SIGALRM, SignalHandler) == SIG_IGN)
    signal (SIGALRM, SIG_IGN);
  if (signal (SIGUSR1, SignalHandler) == SIG_IGN)
    signal (SIGUSR1, SIG_IGN);
  alarm (timeout_accord);

  // on accord la carte
  if ((freq > 100000000))
    {
      if (open_fe (&fds.fd_frontend, card))
	{
	  tune_retval =
	    tune_it (fds.fd_frontend, freq, srate, 0, tone, specInv, diseqc,
		     modulation, HP_CodeRate, TransmissionMode, guardInterval,
		     bandWidth, LP_CodeRate, hier, aff_force_signal);
	}
    }
  else if ((freq != 0) && (pol != 0) && (srate != 0))
    {
      if (open_fe (&fds.fd_frontend, card))
	{
	  fprintf (stderr, "Tuning to %ld Hz\n", freq);
	  tune_retval =
	    tune_it (fds.fd_frontend, freq, srate, pol, tone, specInv, diseqc,
		     modulation, HP_CodeRate, TransmissionMode, guardInterval,
		     bandWidth, LP_CodeRate, hier, aff_force_signal);
	}
    }

  if (tune_retval < 0)
    {
      if (!no_daemon)
	syslog (LOG_USER, "Tunning issue, card %d\n", card);
      else
	fprintf (stderr, "Tunning issue, card %d\n", card);

      exit(ERROR_TUNE);
    }

  carte_accordee = 1;
  // la carte est accord�e, on intercepte les signaux pour fermer proprement
  if (signal (SIGHUP, SignalHandler) == SIG_IGN)
    signal (SIGHUP, SIG_IGN);
  if (signal (SIGINT, SignalHandler) == SIG_IGN)
    signal (SIGINT, SIG_IGN);
  if (signal (SIGTERM, SignalHandler) == SIG_IGN)
    signal (SIGTERM, SIG_IGN);
  alarm (ALARM_TIME);

  //On �crit son PID dans un fichier
  if (!no_daemon)
    {
      sprintf (nom_fich_pid, "/var/run/mumudvb/mumudvb_carte%d.pid", card);
      pidfile = fopen (nom_fich_pid, "w");
      if (pidfile == NULL)
	{
	  syslog (LOG_USER, "%s: %s\n",
		  nom_fich_pid, strerror (errno));
	  exit(ERROR_CREATE_FILE);
	}
      fprintf (pidfile, "%d\n", getpid ());
      fclose (pidfile);
    }

  // we open the file descriptors
  if (create_card_fd (card, nb_flux, num_pids, &fds) < 0)
    return -1;


  // init de la liste des chaines diffusees
  for (curr_channel = 0; curr_channel < nb_flux; curr_channel++)
    {
      chaines_diffuses[curr_channel] = 0;
      chaines_diffuses_old[curr_channel] = 1;
    }

  set_ts_filt (fds.fd_zero, 0, DMX_PES_OTHER);	// PID 0 : Program Association Table (PAT)
  set_ts_filt (fds.fd_EIT, 18, DMX_PES_OTHER);	// PID 18 : Event Information Table (EIT)
  for (curr_channel = 0; curr_channel < nb_flux; curr_channel++)
    {
      for (curr_pid = 0; curr_pid < num_pids[curr_channel]; curr_pid++)
	set_ts_filt (fds.fd[curr_channel][curr_pid], pids[curr_channel][curr_pid], DMX_PES_OTHER);
    }

  gettimeofday (&tv, (struct timezone *) NULL);
  real_start_time = tv.tv_sec;
  now = 0;

  ttl = 2;
  if (!no_daemon)
    syslog (LOG_USER, "Card %d tuned\n", card);
  else
    fprintf (stderr, "Card %d tuned\n", card);

  // Init udp
  for (curr_channel = 0; curr_channel < nb_flux; curr_channel++)
    {
      // le makeclientsocket est pour joindre automatiquement l'ip de multicast cr��e
      // les swiths HP broadcastent les ip de multicast sans clients
      socketOut[curr_channel] = makeclientsocket (ipOut[curr_channel], portOut[curr_channel], ttl, &sOut[curr_channel], no_daemon);
    }

  if (!no_daemon)
    syslog (LOG_USER, "Diffusion %d channel%s\n", nb_flux,
	    (nb_flux == 1 ? "" : "s"));
  else
    fprintf (stderr, "Diffusion %d channel%s\n", nb_flux,
	     (nb_flux == 1 ? "" : "s"));

  for (curr_channel = 0; curr_channel < nb_flux; curr_channel++)
    {
      if (!no_daemon)
	{
	  syslog (LOG_USER, "Channel \"%s\" num %d ip %s:%d\n",
		  noms[curr_channel], curr_channel, ipOut[curr_channel], portOut[curr_channel]);
	}
      else
	{
	  fprintf (stderr,
		   "Channel \"%s\" num %d ip %s:%d\n",
		   noms[curr_channel], curr_channel, ipOut[curr_channel], portOut[curr_channel]);
	  fprintf (stderr, "        pids : ");
	  for (curr_pid = 0; curr_pid < num_pids[curr_channel]; curr_pid++)
	    fprintf (stderr, "%d ", pids[curr_channel][curr_pid]);
	  fprintf (stderr, "\n");
	}
    }


  // Lecture du flux

  pfds[0].fd = fds.fd_dvr;
  pfds[0].events = POLLIN | POLLPRI;
  pfds[1].events = POLLIN | POLLPRI;

  while (!Interrupted)
    {
      /* Poll the open file descriptors */
      poll (pfds, 1, 500);

      {
	/* Attempt to read 188 bytes from /dev/ost/dvr */
	if ((bytes_read = read (fds.fd_dvr, temp_buf, PACKET_SIZE)) > 0)
	  {
	    if (bytes_read != PACKET_SIZE)
	      {
		if (!no_daemon)
		  syslog (LOG_USER, "No bytes left to read - aborting\n");
		else
		  fprintf (stderr, "No bytes left to read - aborting\n");
		break;
	      }

	    pid = ((temp_buf[1] & 0x1f) << 8) | (temp_buf[2]);
	    for (curr_channel = 0; curr_channel < nb_flux; curr_channel++)
	      {
		for (curr_pid = 0; curr_pid < num_pids[curr_channel]; curr_pid++)
		  {
		    // PID 0 : Program Association Table (PAT)
		    // PID 18 : Event Information Table (EIT)
		    if ((pids[curr_channel][curr_pid] == pid) || pid == 0 || pid == 18)
		      {
			// on considere le paquet comme faisant partie d'une chaine
			// uniquement si ce n'est pas le PID 0 ou 18
			if (pid != 0 && pid != 18)
			  chaines_diffuses[curr_channel]++;
			
			// remplissage des buffers
			for (buf_pos = 0; buf_pos < bytes_read; buf_pos++)
			  buf[curr_channel][nb_bytes[curr_channel] + buf_pos] = temp_buf[buf_pos];
			buf[curr_channel][nb_bytes[curr_channel] + 1] =
			  (buf[curr_channel][nb_bytes[curr_channel] + 1] & 0xe0) | hi_mappids[pid];
			buf[curr_channel][nb_bytes[curr_channel] + 2] = lo_mappids[pid];
			nb_bytes[curr_channel] += bytes_read;
			// Buffer plein, on envoie
			if ((nb_bytes[curr_channel] + PACKET_SIZE) > MAX_UDP_SIZE)
			  {
			    sendudp (socketOut[curr_channel], &sOut[curr_channel], buf[curr_channel],
				     nb_bytes[curr_channel]);
			    nb_bytes[curr_channel] = 0;
			  }
		      }
		  }
		count_non_transmis = 0;
		if (alarm_count == 1)
		  {
		    alarm_count = 0;
		    fprintf (stderr,
			     "Good, we receive back significant data\n");
		  }
	      }
	    count_non_transmis++;
	    if (count_non_transmis > ALARM_COUNT_LIMIT)
	      {
		if (!no_daemon)
		  syslog (LOG_USER,
			  "Error : less than one paquet on %d sent\n",
			  ALARM_COUNT_LIMIT);
		else
		  fprintf (stderr,
			  "Error : less than one paquet on %d sent\n",
			   ALARM_COUNT_LIMIT);
		alarm_count = 1;
	      }
	  }

      }
    }

  if (Interrupted)
    {
      if (!no_daemon)
	syslog (LOG_USER, "\nCaught signal %d - closing cleanly.\n",
		Interrupted);
      else
	fprintf (stderr, "\nCaught signal %d - closing cleanly.\n",
		 Interrupted);
    }

  for (curr_channel = 0; curr_channel < nb_flux; curr_channel++)
    close (socketOut[curr_channel]);

  // we close the file descriptors
  close_card_fd (card, nb_flux, num_pids, fds);
  close (fds.fd_frontend);

  if (remove (nom_fich_chaines_diff))
    {
      if (!no_daemon)
	syslog (LOG_USER,
		"%s: %s\n",
		nom_fich_chaines_diff, strerror (errno));
      else
	fprintf (stderr,
		 "%s: %s\n",
		 nom_fich_chaines_diff, strerror (errno));
      exit(ERROR_DEL_FILE);
    }


  if (!no_daemon)
    {
      if (remove (nom_fich_pid))
	{
	  syslog (LOG_USER, "%s: %s\n",
		  nom_fich_pid, strerror (errno));
	  exit(ERROR_DEL_FILE);
	}
    }

  return (0);
}


static void
SignalHandler (int signum)
{
  struct timeval tv;
  int curr_channel = 0;
  int compteur_chaines_diff=0;

  if (signum == SIGALRM)
    {
      gettimeofday (&tv, (struct timezone *) NULL);
      now = tv.tv_sec - real_start_time;
      if (aff_force_signal && carte_accordee)
	affiche_puissance (fds, no_daemon);
      if (!carte_accordee)
	{
	  if (!no_daemon)
	    syslog (LOG_USER,
		    "Card not tuned after %ds - exiting\n",
		    timeout_accord);
	  else
	    fprintf (stderr,
		    "Card not tuned after %ds - exiting\n",
		     timeout_accord);
	  exit(ERROR_TUNE);
	}	
      for (curr_channel = 0; curr_channel < nb_flux; curr_channel++)
	if ((chaines_diffuses[curr_channel] >= 100) && (!chaines_diffuses_old[curr_channel]))
	  {
	    if (!no_daemon)
	      syslog (LOG_USER,
		      "Channel \"%s\" back.Card %d\n",
		      noms[curr_channel], card);
	    else
	      fprintf (stderr,
		      "Channel \"%s\" back.Card %d\n",
		       noms[curr_channel], card);
	    chaines_diffuses_old[curr_channel] = 1;	// update
	  }
	else if ((chaines_diffuses_old[curr_channel]) && (chaines_diffuses[curr_channel] < 100))
	  {
	    if (!no_daemon)
	      syslog (LOG_USER,
		      "Channel \"%s\" down.Card %d\n",
		      noms[curr_channel], card);
	    else
	      fprintf (stderr,
		      "Channel \"%s\" down.Card %d\n",
		      noms[curr_channel], card);
	    chaines_diffuses_old[curr_channel] = 0;	// update
	  }

      // on compte les chaines diffuses
      for (curr_channel = 0; curr_channel < nb_flux; curr_channel++)
	if (chaines_diffuses_old[curr_channel])
	  compteur_chaines_diff++;

      // reinit si on diffuse
      if(compteur_chaines_diff)
	time_no_diff=0;
      // sinon si c le moment ou on arrete on stoque l'heure
      else if(!time_no_diff)
	time_no_diff=now;

      // on ne diffuse plus depuis trop longtemps
      if(time_no_diff&&((now-time_no_diff)>timeout_no_diff))
	{
	  if (!no_daemon)
	    syslog (LOG_USER,
		    "No data from card %d in %ds, exiting.\n",
		      card, timeout_no_diff);
	    else
	      fprintf (stderr,
		    "No data from card %d in %ds, exiting.\n",
		      card, timeout_no_diff);
	  Interrupted=ERROR_NO_DIFF;
	}

      // on envoie le old pour annoncer que les chaines
      // qui diffusent au dessus du quota de pauqets
      gen_chaines_diff (no_daemon, chaines_diffuses_old);

      // reinit
      for (curr_channel = 0; curr_channel < nb_flux; curr_channel++)
	chaines_diffuses[curr_channel] = 0;
      alarm (ALARM_TIME);
    }
  else if (signum == SIGUSR1)
    {
      aff_force_signal = aff_force_signal ? 0 : 1;
    }
  else if (signum != SIGPIPE)
    {
      Interrupted = signum;
    }
  signal (signum, SignalHandler);
}


void
gen_chaines_diff (int no_daemon, int *chaines_diffuses)
{
  FILE *chaines_diff;
  int curr_channel;

  chaines_diff = fopen (nom_fich_chaines_diff, "w");
  if (chaines_diff == NULL)
    {
      if (!no_daemon)
	syslog (LOG_USER,
		"%s: %s\n",
		nom_fich_chaines_diff, strerror (errno));
      else
	fprintf (stderr,
		 "%s: %s\n",
		 nom_fich_chaines_diff, strerror (errno));
      exit(ERROR_CREATE_FILE);
    }

  for (curr_channel = 0; curr_channel < nb_flux; curr_channel++)
    if (chaines_diffuses[curr_channel])
      fprintf (chaines_diff, "%s:%d:%s\n", ipOut[curr_channel], portOut[curr_channel], noms[curr_channel]);
  fclose (chaines_diff);

}
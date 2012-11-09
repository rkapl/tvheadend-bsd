/*
 *  TV Input - Linux DVB interface
 *  Copyright (C) 2012 Andreas Öman
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * DVB input using hardware filters
 */
#include <assert.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dvb/frontend.h>
#include <linux/dvb/dmx.h>
#include <config.h>
#if EPOLL_ENABLED
#include <sys/epoll.h>
#else
#include<poll.h>
#endif

#include "tvheadend.h"
#include "dvb.h"
#include "service.h"

/**
 * Install filters for a service
 *
 * global_lock must be held
 */
static void
open_service(th_dvb_adapter_t *tda, service_t *s)
{
  struct dmx_pes_filter_params dmx_param;
  int fd;
  elementary_stream_t *st;

  TAILQ_FOREACH(st, &s->s_components, es_link) {
    if(st->es_pid >= 0x2000)
      continue;

    if(st->es_demuxer_fd != -1)
      continue;

    fd = tvh_open(tda->tda_demux_path, O_RDWR, 0);
    st->es_cc_valid = 0;

    if(fd == -1) {
      st->es_demuxer_fd = -1;
      tvhlog(LOG_ERR, "dvb",
	     "\"%s\" unable to open demuxer \"%s\" for pid %d -- %s",
	     s->s_identifier, tda->tda_demux_path, 
	     st->es_pid, strerror(errno));
      continue;
    }

    memset(&dmx_param, 0, sizeof(dmx_param));
    dmx_param.pid = st->es_pid;
    dmx_param.input = DMX_IN_FRONTEND;
    dmx_param.output = DMX_OUT_TS_TAP;
    dmx_param.pes_type = DMX_PES_OTHER;
    dmx_param.flags = DMX_IMMEDIATE_START;

    if(ioctl(fd, DMX_SET_PES_FILTER, &dmx_param)) {
      tvhlog(LOG_ERR, "dvb",
	     "\"%s\" unable to configure demuxer \"%s\" for pid %d -- %s",
	     s->s_identifier, tda->tda_demux_path, 
	     st->es_pid, strerror(errno));
      close(fd);
      fd = -1;
    }

    st->es_demuxer_fd = fd;
  }
}


/**
 * Remove filters for a service
 *
 * global_lock must be held
 */
static void
close_service(th_dvb_adapter_t *tda, service_t *s)
{
  elementary_stream_t *es;

  TAILQ_FOREACH(es, &s->s_components, es_link) {
    if(es->es_demuxer_fd != -1) {
      close(es->es_demuxer_fd);
      es->es_demuxer_fd = -1;
    }
  }
}




/**
 *
 */
static void
open_table(th_dvb_mux_instance_t *tdmi, th_dvb_table_t *tdt)
{
  th_dvb_adapter_t *tda = tdmi->tdmi_adapter;
#if EPOLL_ENABLED
  struct epoll_event e;
#endif
  static int tdt_id_tally;

  tdt->tdt_fd = tvh_open(tda->tda_demux_path, O_RDWR, 0);

  if(tdt->tdt_fd != -1) {

    tdt->tdt_id = ++tdt_id_tally;
#if EPOLL_ENABLED
    e.events = EPOLLIN;
    e.data.u64 = ((uint64_t)tdt->tdt_fd << 32) | tdt->tdt_id;

    if(epoll_ctl(tda->tda_table_epollfd, EPOLL_CTL_ADD, tdt->tdt_fd, &e)) {
      close(tdt->tdt_fd);
      tdt->tdt_fd = -1;
    } else {
#else
    write(tda->tda_table_poll_wakeup_pipe[1],"a",1);
#endif
      struct dmx_sct_filter_params fp = {0};
  
      fp.filter.filter[0] = tdt->tdt_table;
      fp.filter.mask[0]   = tdt->tdt_mask;

      if(tdt->tdt_flags & TDT_CRC)
	fp.flags |= DMX_CHECK_CRC;
      fp.flags |= DMX_IMMEDIATE_START;
      fp.pid = tdt->tdt_pid;

      if(ioctl(tdt->tdt_fd, DMX_SET_FILTER, &fp)) {
	close(tdt->tdt_fd);
	tdt->tdt_fd = -1;
      }
#if EPOLL_ENABLED
    }
#endif
  }

  if(tdt->tdt_fd == -1)
    TAILQ_INSERT_TAIL(&tdmi->tdmi_table_queue, tdt, tdt_pending_link);
}


/**
 * Close FD for the given table and put table on the pending list
 */
static void
tdt_close_fd(th_dvb_mux_instance_t *tdmi, th_dvb_table_t *tdt)
{

  th_dvb_adapter_t *tda = tdmi->tdmi_adapter;
  assert(tdt->tdt_fd != -1);
#ifdef EPOLL_ENABLED
  epoll_ctl(tda->tda_table_epollfd, EPOLL_CTL_DEL, tdt->tdt_fd, NULL);
#else
  write(tda->tda_table_poll_wakeup_pipe[1],"f",1);
#endif
  close(tdt->tdt_fd);

  tdt->tdt_fd = -1;
  TAILQ_INSERT_TAIL(&tdmi->tdmi_table_queue, tdt, tdt_pending_link);
}


static void
dvb_table_cycle(th_dvb_mux_instance_t* tdmi,th_dvb_table_t* tdt,int64_t* cycle_barrier)
{
  /* Any tables pending (that wants a filter/fd), close this one */
  if(TAILQ_FIRST(&tdmi->tdmi_table_queue) != NULL &&
    *cycle_barrier < getmonoclock()) {
    tdt_close_fd(tdmi, tdt);
    *cycle_barrier = getmonoclock() + 100000;
    tdt = TAILQ_FIRST(&tdmi->tdmi_table_queue);
    assert(tdt != NULL);
    TAILQ_REMOVE(&tdmi->tdmi_table_queue, tdt, tdt_pending_link);

    open_table(tdmi, tdt);
  }
}

#if ENABLE_EPOLL
static void *
dvb_table_input(void *aux)
{
  th_dvb_adapter_t *tda = aux;
  int r, i, tid, fd, x;
  struct epoll_event ev[1];
  uint8_t sec[4096];
  th_dvb_mux_instance_t *tdmi;
  th_dvb_table_t *tdt;
  int64_t cycle_barrier = 0; 

  while(1) {
    x = epoll_wait(tda->tda_table_epollfd, ev, sizeof(ev) / sizeof(ev[0]), -1);

    for(i = 0; i < x; i++) {
    
      tid = ev[i].data.u64 & 0xffffffff;
      fd  = ev[i].data.u64 >> 32; 

      if(!(ev[i].events & EPOLLIN))
	continue;

      if((r = read(fd, sec, sizeof(sec))) < 3)
	continue;

      pthread_mutex_lock(&global_lock);
      if((tdmi = tda->tda_mux_current) != NULL) {
	LIST_FOREACH(tdt, &tdmi->tdmi_tables, tdt_link)
	  if(tdt->tdt_id == tid)
	    break;

	if(tdt != NULL) {
	  dvb_table_dispatch(sec, r, tdt);
	  dvb_table_cycle(tdmi,tdt,&cycle_barrier);
	}
      }
      pthread_mutex_unlock(&global_lock);
    }
  }
  return NULL;
}
#else
static void*
dvb_table_input(void *aux){
  th_dvb_adapter_t *tda = aux;
  /* We are going to listen on all the tables using poll and on the control pipe
   * When we receive a character from a control pipe, we rebuild our list of tables to poll */
  int i,r;
  uint8_t sec[4096];
  int64_t cycle_barrier=0;
  struct pollfd* poll_list=NULL;
  int poll_count=0; /*number of tables to poll, excluding the control pipe*/
  th_dvb_table_t** poll_mapping=NULL;
  th_dvb_table_t* tdt;
  th_dvb_mux_instance_t* tdmi=NULL;
  while(1){
      if(poll_list==NULL){ /* re-read the tables */
          pthread_mutex_lock(&global_lock);
          /* When switching muxes, all tables are flushed, so it is safe to wait
           * just on the control pipe, if there is no current mux. Default table will be added
           * after switching the mux*/
          if((tdmi = tda->tda_mux_current) != NULL) {
            poll_count=0;
            LIST_FOREACH(tdt, &tdmi->tdmi_tables, tdt_link)
               poll_count++;
            poll_list=malloc(sizeof(struct pollfd)*(poll_count+1));
            poll_mapping=malloc(sizeof(th_dvb_table_t*)*poll_count);
            poll_count=0;
            LIST_FOREACH(tdt, &tdmi->tdmi_tables, tdt_link){
              poll_list[poll_count+1].events=POLLIN;
              poll_list[poll_count+1].fd=tdt->tdt_fd;
              poll_mapping[poll_count]=tdt;
              poll_count++;
            }
          }else{ /* no muxes to listen on */
              poll_count=0;
              poll_list=malloc(sizeof(struct pollfd)*1);
          }
          poll_list[0].events=POLLIN;
          poll_list[0].fd=tda->tda_table_poll_wakeup_pipe[0];
          pthread_mutex_unlock(&global_lock);
      }
      for(i=0;i<poll_count+1;i++) poll_list[i].revents=0;
      if(poll(poll_list,poll_count+1,-1)==-1){
          perror("poll");
      }
      if(poll_list[0].revents!=0){/* events on control pipe - drop */
          char buf;
          read(poll_list[0].fd,&buf,1);
          free(poll_list);
          free(poll_mapping);
          poll_list=NULL;
          poll_mapping=NULL;
      }else{ //read data from tables
          for(i=1;i<poll_count+1;i++){
              if((poll_list[i].revents & POLLIN)!=0){
                  tdt=poll_mapping[i-1];
                  if((r = read(tdt->tdt_fd, sec, sizeof(sec))) < 3)
                    continue;
                  pthread_mutex_lock(&global_lock);
                  dvb_table_dispatch(sec, r, tdt);
                  dvb_table_cycle(tdmi,tdt,&cycle_barrier);
                  pthread_mutex_unlock(&global_lock);
              }
          }
      }
  }
  return NULL;
}
#endif


static void
close_table(th_dvb_mux_instance_t *tdmi, th_dvb_table_t *tdt)
{
  th_dvb_adapter_t *tda = tdmi->tdmi_adapter;
  if(tdt->tdt_fd == -1) {
    TAILQ_REMOVE(&tdmi->tdmi_table_queue, tdt, tdt_pending_link);
  } else {
#if EPOLL_ENABLED
    epoll_ctl(tda->tda_table_epollfd, EPOLL_CTL_DEL, tdt->tdt_fd, NULL);
#else
    write(tda->tda_table_poll_wakeup_pipe[1],"c",1);
#endif
    close(tdt->tdt_fd);
  }
}

/**
 *
 */
void
dvb_input_filtered_setup(th_dvb_adapter_t *tda)
{
  tda->tda_open_service  = open_service;
  tda->tda_close_service = close_service;
  tda->tda_open_table    = open_table;
  tda->tda_close_table   = close_table;

  pthread_t ptid;
#if EPOLL_ENABLED
  tda->tda_table_epollfd = epoll_create(50);
#else
  pipe(tda->tda_table_poll_wakeup_pipe);
#endif
  pthread_create(&ptid, NULL, dvb_table_input, tda);
}


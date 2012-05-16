/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2005 Mellanox Technologies Ltd.  All rights reserved.
 * Copyright (c) 2005 Hewlett Packard, Inc (Grant Grundler)
 * Copyright (c) 2009 HNR Consulting.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * $Id$
 */

#if HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <malloc.h>
#include <signal.h>

#include "get_clock.h"
#include "perftest_resources.h"
#include "perftest_parameters.h"
#include "perftest_communication.h"
#include "version.h"

cycles_t *tstamp;
volatile cycles_t	start_traffic = 0;
volatile cycles_t	end_traffic = 0;
volatile cycles_t	start_sample = 0;
volatile cycles_t	end_sample = 0;
cycles_t  *tstamp;
struct perftest_parameters user_param;

/****************************************************************************** 
 *
 ******************************************************************************/
static int pp_connect_ctx(struct pingpong_context *ctx,int my_psn,
						  struct pingpong_dest *dest,int my_reads,
						  struct perftest_parameters *user_parm)
{
	struct ibv_qp_attr attr;
	memset(&attr, 0, sizeof(struct ibv_qp_attr));

	attr.qp_state               = IBV_QPS_RTR;
	attr.path_mtu				= user_parm->curr_mtu;
	attr.dest_qp_num            = dest->qpn;
	attr.rq_psn                 = dest->psn;
	attr.ah_attr.dlid           = dest->lid;
	attr.max_dest_rd_atomic     = my_reads;
	attr.min_rnr_timer          = 12;
	if (user_parm->gid_index < 0) {
		attr.ah_attr.is_global      = 0;
		attr.ah_attr.sl             = user_parm->sl;
	} else {
		attr.ah_attr.is_global      = 1;
		attr.ah_attr.grh.dgid       = dest->gid;
		attr.ah_attr.grh.sgid_index = user_parm->gid_index;
		attr.ah_attr.grh.hop_limit  = 1;
		attr.ah_attr.sl             = 0;
	}
	attr.ah_attr.src_path_bits  = 0;
	attr.ah_attr.port_num       = user_parm->ib_port;
	if (ibv_modify_qp(ctx->qp[0], &attr,
			  IBV_QP_STATE              |
			  IBV_QP_AV                 |
			  IBV_QP_PATH_MTU           |
			  IBV_QP_DEST_QPN           |
			  IBV_QP_RQ_PSN             |
			  IBV_QP_MIN_RNR_TIMER      |
			  IBV_QP_MAX_DEST_RD_ATOMIC)) {
		fprintf(stderr, "Failed to modify RC QP to RTR\n");
		return 1;
	}
	attr.timeout            = user_parm->qp_timeout;
	attr.retry_cnt          = 7;
	attr.rnr_retry          = 7;
	attr.qp_state           = IBV_QPS_RTS;
	attr.max_rd_atomic      = dest->out_reads;
	attr.sq_psn             = my_psn;

	if (ibv_modify_qp(ctx->qp[0], &attr,
				  IBV_QP_STATE              |
				  IBV_QP_SQ_PSN             |
				  IBV_QP_TIMEOUT            |
				  IBV_QP_RETRY_CNT          |
				  IBV_QP_RNR_RETRY          |
				  IBV_QP_MAX_QP_RD_ATOMIC)) {
			fprintf(stderr, "Failed to modify RC QP to RTS\n");
			return 1;
	}
	return 0;
}

/*
 * When there is an
 *	odd number of samples, the median is the middle number.
 * 	even number of samples, the median is the mean of the
 *		two middle numbers.
 *
 */
static inline cycles_t get_median(int n, cycles_t delta[])
{
	if ((n - 1) % 2)
		return (delta[n / 2] + delta[n / 2 - 1]) / 2;
	else
		return delta[n / 2];
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int cycles_compare(const void * aptr, const void * bptr)
{
	const cycles_t *a = aptr;
	const cycles_t *b = bptr;
	if (*a < *b) return -1;
	if (*a > *b) return 1;
	return 0;

}

/****************************************************************************** 
 *
 ******************************************************************************/
static void print_report(struct perftest_parameters *user_param, int total_sent, int start_index) {

	double cycles_to_units;
	cycles_t median;
	unsigned int i, a;
	const char* units;
	int statistic_space_size = (total_sent < user_param->iters) ? total_sent : user_param->iters ;
	cycles_t *delta = malloc((statistic_space_size - 1) * sizeof *delta);

	if (!delta) {
		perror("malloc");
		return;
	}
		
	for (a = 0, i = start_index; a < statistic_space_size - 1; ++a, i = (i + 1) % statistic_space_size) {
		delta[a] = tstamp[(i + 1) % statistic_space_size] - tstamp[i];
	}

	if (user_param->r_flag->cycles) {
		cycles_to_units = 1;
		units = "cycles";
	} else {
		cycles_to_units = get_cpu_mhz(user_param->cpu_freq_f);
		units = "usec";
	}

	if (user_param->r_flag->unsorted) {
		printf("#, %s\n", units);
		for (i = 0; i < statistic_space_size - 1; ++i)
			printf("%d, %g\n", i + 1, delta[i] / cycles_to_units );
	}

	qsort(delta, statistic_space_size - 1, sizeof *delta, cycles_compare);

	if (user_param->r_flag->histogram) {
		printf("#, %s\n", units);
		for (i = 0; i < statistic_space_size - 1; ++i)
			printf("%d, %g\n", i + 1, delta[i] / cycles_to_units );
	}

	median = get_median(statistic_space_size - 1, delta);

	printf(REPORT_FMT_LAT,(unsigned long)user_param->size,statistic_space_size,delta[1] / cycles_to_units ,
	       delta[statistic_space_size - 3] / cycles_to_units ,median / cycles_to_units );

	free(delta);
}

/****************************************************************************** 
 *
 ******************************************************************************/
int run_iter(struct pingpong_context *ctx, 
			 struct perftest_parameters *user_param,
			 struct pingpong_dest *rem_dest) {

	int 					scnt = 0;
	int 					ne = 0;
	int					sample_scnt=0;
	struct ibv_sge 				list;
	struct ibv_send_wr 			wr;
	struct ibv_send_wr  			*bad_wr;
	struct ibv_wc       			wc;
	uint64_t            			my_addr,rem_addr;

	memset(&wr,0,sizeof(struct ibv_send_wr));

	list.addr   = (uintptr_t)ctx->buf;
	list.length = user_param->size;
	list.lkey   = ctx->mr->lkey;

	wr.sg_list             = &list;
	wr.wr.rdma.remote_addr = rem_dest->vaddr;
	wr.wr.rdma.rkey        = rem_dest->rkey;
	wr.wr_id               = PINGPONG_READ_WRID;
	wr.num_sge             = MAX_RECV_SGE;
	wr.opcode              = IBV_WR_RDMA_READ;
	wr.send_flags          = IBV_SEND_SIGNALED;
	wr.next                = NULL;

	my_addr  = list.addr;
	rem_addr = wr.wr.rdma.remote_addr;
	
	while (scnt < user_param->iters || (user_param->test_type == DURATION && user_param->state != END_STATE)) {
	
		if (user_param->state == END_STATE) break;

		if (user_param->test_type == ITERATIONS) {
			tstamp[scnt] = get_cycles();
		}else if (user_param->state == SAMPLE_STATE) {
				tstamp[sample_scnt++ % user_param->iters] = get_cycles();
		}

		if (ibv_post_send(ctx->qp[0],&wr,&bad_wr)) {
			fprintf(stderr, "Couldn't post send: scnt=%d\n",scnt);
			return 11;
		}

		if (user_param->size <= (CYCLE_BUFFER / 2)) { 
			increase_rem_addr(&wr,user_param->size,scnt,rem_addr);
			increase_loc_addr(&list,user_param->size,scnt,my_addr,0);
		}

		scnt++;
	
		if (user_param->use_event) {
			if (ctx_notify_events(ctx->cq,ctx->channel)) {
				fprintf(stderr, "Couldn't request CQ notification\n");
				return 1;
			}
		}

		do {
			if (user_param->state == END_STATE) break;
			ne = ibv_poll_cq(ctx->cq, 1, &wc);
			if(ne > 0) { 
				if (wc.status != IBV_WC_SUCCESS) 
					NOTIFY_COMP_ERROR_SEND(wc,scnt,scnt);
			}
		} while (!user_param->use_event && ne == 0);

		if (ne < 0) {
			fprintf(stderr, "poll CQ failed %d\n", ne);
			return 12;
		}
		
	}
	end_traffic = get_cycles();
	scnt = (user_param->test_type == DURATION) ? sample_scnt : scnt ;
	print_report(user_param, scnt, (user_param->state == END_STATE && scnt >  user_param->iters) ? ((scnt + 1) % user_param->iters) : 0);
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int main(int argc, char *argv[]) {

	int                         i = 0;
	int                         size_min_pow = 1;
	int                         size_max_pow = 24;
	struct report_options       report = {};
	struct pingpong_context     ctx;
	struct ibv_device           *ib_dev;
	struct pingpong_dest	    my_dest,rem_dest;
	struct perftest_comm		user_comm;
	
	/* init default values to user's parameters */
	memset(&ctx,0,sizeof(struct pingpong_context));
	memset(&user_param,0,sizeof(struct perftest_parameters));
	memset(&user_comm,0,sizeof(struct perftest_comm));
	memset(&my_dest,0,sizeof(struct pingpong_dest));
	memset(&rem_dest,0,sizeof(struct pingpong_dest));

	user_param.verb    = READ;
	user_param.tst     = LAT;
	user_param.r_flag  = &report;
	user_param.version = VERSION;

	// Configure the parameters values according to user arguments or defalut values.
	if (parser(&user_param,argv,argc)) {
		fprintf(stderr," Parser function exited with Error\n");
		return FAILURE;
	}

	// Finding the IB device selected (or defalut if no selected).
	ib_dev = ctx_find_dev(user_param.ib_devname);
	if (!ib_dev) {
		fprintf(stderr," Unable to find the Infiniband/RoCE deivce\n");
		return FAILURE;
	}

	// Getting the relevant context from the device
	ctx.context = ibv_open_device(ib_dev);
	if (!ctx.context) {
		fprintf(stderr, " Couldn't get context for the device\n");
		return 1;
	}

	// See if MTU and link type are valid and supported.
	if (check_link_and_mtu(ctx.context,&user_param)) {
		fprintf(stderr, " Couldn't get context for the device\n");
		return FAILURE;
	}

	// Print basic test information.
	ctx_print_test_info(&user_param);

	i=size_min_pow;
	if (user_param.connection_type == UD)  
 		size_max_pow =  (int)UD_MSG_2_EXP(MTU_SIZE(user_param.curr_mtu)) ;
 
	do { 

		if (user_param.all == ON) 
 			user_param.size = 1 << i;

		// copy the rellevant user parameters to the comm struct + creating rdma_cm resources.
		if (create_comm_struct(&user_comm,&user_param)) { 
			fprintf(stderr," Unable to create RDMA_CM resources\n");
			return 1;
		}

		// Create (if nessacery) the rdma_cm ids and channel.
		if (user_param.work_rdma_cm == ON) {

			if (create_rdma_resources(&ctx,&user_param)) {
				fprintf(stderr," Unable to create the rdma_resources\n");
				return FAILURE;
			}

			if (user_param.machine == CLIENT) {

				if (rdma_client_connect(&ctx,&user_param)) {
					fprintf(stderr,"Unable to perform rdma_client function\n");
					return FAILURE;
				}
		
			} else {

				if (rdma_server_connect(&ctx,&user_param)) {
					fprintf(stderr,"Unable to perform rdma_client function\n");
					return FAILURE;
				}
			}

		} else {

			// create all the basic IB resources (data buffer, PD, MR, CQ and events channel)
	   		 if (ctx_init(&ctx,&user_param)) {
				fprintf(stderr, " Couldn't create IB resources\n");
				return FAILURE;
	   		 }
		}

		// Set up the Connection.
		if (set_up_connection(&ctx,&user_param,&my_dest)) {
			fprintf(stderr," Unable to set up socket connection\n");
			return 1;
		} 
		if (user_param.all == OFF || (user_param.all == ON && i == size_min_pow)) 
			ctx_print_pingpong_data(&my_dest,&user_comm);

		if (user_param.all == OFF || (user_param.all == ON && i == size_min_pow) || user_param.machine == CLIENT || user_param.work_rdma_cm == ON) {
			// Init the connection and print the local data.
			if (establish_connection(&user_comm)) {
				fprintf(stderr," Unable to init the socket connection\n");
				return 1;
			}
		} else { //if not the first time, server is waiting for client
			user_comm.rdma_params->sockfd = accept(user_comm.sockfd_sd, NULL, 0);
			if (user_comm.rdma_params->sockfd < 0) {
				fprintf(stderr, "accept() failed\n");
				close(user_comm.sockfd_sd);
				return 1;
			}
		}

		//  shaking hands and gather the other side info.
		if (ctx_hand_shake(&user_comm,&my_dest,&rem_dest)) {
			fprintf(stderr,"Failed to exchange date between server and clients\n");
			return 1;
		}
		if (user_param.all == OFF || (user_param.all == ON && i == size_min_pow)) {
			user_comm.rdma_params->side = REMOTE;
			ctx_print_pingpong_data(&rem_dest,&user_comm);
			printf(RESULT_LINE);
			printf(RESULT_FMT_LAT);
		}

		if (user_param.work_rdma_cm == OFF) {

			if (pp_connect_ctx(&ctx,my_dest.psn,&rem_dest,my_dest.out_reads,&user_param)) {
				fprintf(stderr," Unable to Connect the HCA's through the link\n");
				return 1;
			}
		}

		// An additional handshake is required after moving qp to RTR.
		if (ctx_hand_shake(&user_comm,&my_dest,&rem_dest)) {
       			fprintf(stderr,"Failed to exchange date between server and clients\n");
       			return 1;
    		}

		ALLOCATE(tstamp,cycles_t,user_param.iters);

	
		// Only Client post read request. 
		if (user_param.machine == CLIENT) {

			if (user_param.use_event) {
				if (ibv_req_notify_cq(ctx.cq, 0)) {
					fprintf(stderr, "Couldn't request CQ notification\n");
					return 1;
				} 
			}

			if (user_param.test_type == DURATION) {
					user_param.state = START_STATE;
					signal(SIGALRM, catch_alarm);
					alarm(user_param.margin);
			}
			start_traffic = get_cycles();
	
			if(run_iter(&ctx,&user_param,&rem_dest))
				return 18;
	
		}	

		if (ctx_close_connection(&user_comm,&my_dest,&rem_dest)) {
	 		fprintf(stderr,"Failed to close connection between server and client\n");
	 		return 1;
		}

			if (destroy_ctx(&ctx, &user_param, NULL, 0)){
			fprintf(stderr,"Failed to destroy_ctx\n");
        		return 1;
		}
		if (user_param.use_rdma_cm) {
			if (destroy_ctx(user_comm.rdma_ctx, user_comm.rdma_params , NULL, ((user_param.all == ON && i == size_max_pow) || user_param.all == OFF))){
				fprintf(stderr,"Failed to destroy_ctx\n");
        			return 1;
			}
		} 
		i++;
	}while ((i >= size_min_pow) && (i <= size_max_pow) && (user_param.all == ON));
	printf(RESULT_LINE);

	return 0; // destroy_ctx(&ctx,&user_param);
}

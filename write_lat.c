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

volatile cycles_t	start_traffic = 0;
volatile cycles_t	end_traffic = 0;
volatile cycles_t	start_sample = 0;
volatile cycles_t	end_sample = 0;
cycles_t  *tstamp;
cycles_t  *t_first_stamp;

struct perftest_parameters user_param;

/****************************************************************************** 
 *
 ******************************************************************************/
static int pp_connect_ctx(struct pingpong_context *ctx,int my_psn,
						  struct pingpong_dest *dest,
						  struct perftest_parameters *user_parm)
{
	struct ibv_qp_attr attr;
	memset(&attr, 0, sizeof(struct ibv_qp_attr));

	attr.qp_state         = IBV_QPS_RTR;
	attr.path_mtu         = user_parm->curr_mtu;
	attr.dest_qp_num      = dest->qpn;
	attr.rq_psn           = dest->psn;
	attr.ah_attr.dlid     = dest->lid;

	if (user_parm->connection_type == RC) {
		attr.max_dest_rd_atomic     = 1;
		attr.min_rnr_timer          = 12;
	}

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

	if (user_parm->connection_type == RC) {
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
	} else {
		if (ibv_modify_qp(ctx->qp[0], &attr,
				  IBV_QP_STATE              |
				  IBV_QP_AV                 |
				  IBV_QP_PATH_MTU           |
				  IBV_QP_DEST_QPN           |
				  IBV_QP_RQ_PSN)) {
			fprintf(stderr, "Failed to modify UC QP to RTR\n");
			return 1;
		}

	}
	attr.qp_state             = IBV_QPS_RTS;
	attr.sq_psn       = my_psn;

	if (user_parm->connection_type == RC) {
		attr.max_rd_atomic  = 1;
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
	} else {
		if (ibv_modify_qp(ctx->qp[0], &attr,
				  IBV_QP_STATE              |
				  IBV_QP_SQ_PSN)) {
			fprintf(stderr, "Failed to modify UC QP to RTS\n");
			return 1;
		}

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
		return(delta[n / 2] + delta[n / 2 - 1]) / 2;
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
static void print_report(struct perftest_comm *comm, struct perftest_parameters *user_param, int total_sent, int start_index) {

	double cycles_to_units;

	cycles_t *p_cycles_to_units = (cycles_t *)malloc(sizeof(cycles_t));
	cycles_t *p_other_side_cycles_to_units = (cycles_t *)malloc(sizeof(cycles_t));

	cycles_t median;
	unsigned int i, a;
	const char* units;
	int statistic_space_size = (total_sent < user_param->iters) ? total_sent : user_param->iters ;
	cycles_t *delta = (cycles_t *)malloc((statistic_space_size - 1) * sizeof(cycles_t));
	cycles_t *other_side_delta_first2last_delta = (cycles_t *)malloc((statistic_space_size - 1) * sizeof(cycles_t));
	cycles_t *first2last_byte_delta = (cycles_t *)malloc((statistic_space_size - 1) * sizeof(cycles_t));

	if (!delta) {
		perror("malloc");
		return;
	}
	if (user_param->r_flag->cycles) {
		cycles_to_units = 1;
		units = "cycles";
	} else {
		cycles_to_units = get_cpu_mhz(user_param->cpu_freq_f);
		units = "usec";
	}

	if (user_param->calc_first_byte_latency){
		*p_cycles_to_units=(cycles_t)(cycles_to_units*1000000);
		if (change_data(comm, p_cycles_to_units, p_other_side_cycles_to_units)){
				printf("change data problem\n");		
		}
	}

	for (a = 0, i = start_index; a < statistic_space_size - 1; ++a, i = (i + 1) % statistic_space_size) {
		if (user_param->calc_first_byte_latency){
			first2last_byte_delta[a] = tstamp[i]-t_first_stamp[i];
			if (change_data(comm, first2last_byte_delta+i, other_side_delta_first2last_delta+i)){
				printf("change data problem\n");		
			}

			// normalize other side delta according to frequency
			other_side_delta_first2last_delta[a]= (unsigned long long)(round((double)(other_side_delta_first2last_delta[a])/(double)(*p_other_side_cycles_to_units)*(double)(*p_cycles_to_units)));

			delta[a]=(int)(tstamp[(i + 1) % statistic_space_size] - tstamp[i]);
			
		}
		else
			delta[a] = tstamp[(i + 1) % statistic_space_size] - tstamp[i];
		
	}

	if (user_param->calc_first_byte_latency){
		for (a = 0, i = start_index; a < statistic_space_size - 1; ++a, i = (i + 1) % statistic_space_size) {
			if (user_param->machine == SERVER)
// 				delta[a] = delta[a] - first2last_byte_delta[i]-other_side_delta_first2last_delta[i+1];
				delta[a] = delta[a] - first2last_byte_delta[a]-other_side_delta_first2last_delta[a+1];
			else
// 				delta[a] = delta[a] - first2last_byte_delta[i+1]-other_side_delta_first2last_delta[i+1];
				delta[a] = delta[a] - first2last_byte_delta[a+1]-other_side_delta_first2last_delta[a+1];

		}
	}

	if (user_param->r_flag->unsorted) {
		printf("#, %s\n", units);
		for (i = 0; i < statistic_space_size - 1; ++i)
			printf("%d, %g\n", i + 1, delta[i] / cycles_to_units / 2);
	}

	qsort(delta, statistic_space_size - 1, sizeof *delta, cycles_compare);

	if (user_param->r_flag->histogram) {
		printf("#, %s\n", units);
		for (i = 0; i < statistic_space_size - 1; ++i)
			printf("%d, %g\n", i + 1, delta[i] / cycles_to_units / 2);
	}

	median = get_median(statistic_space_size - 1, delta);

	printf(REPORT_FMT_LAT,(unsigned long)user_param->size,statistic_space_size,delta[1] / cycles_to_units / 2,
       		delta[statistic_space_size - 10] / cycles_to_units / 2,median / cycles_to_units / 2);
	if (user_param->test_type == ITERATIONS) { //bug - in DURATION mode
	  	free(delta);
	  	free(first2last_byte_delta);
	  	free(other_side_delta_first2last_delta);
	  	free(p_cycles_to_units);
	  	free(p_other_side_cycles_to_units);
	}
}

/****************************************************************************** 
 *
 ******************************************************************************/
int run_iter(struct perftest_comm *comm, struct pingpong_context *ctx, 
			 struct perftest_parameters *user_param,
	         struct pingpong_dest *rem_dest) {

	int 					scnt = 0;
	int 					ccnt = 0;
	int 					rcnt = 0;
	int                     		ne;
	int					sample_scnt=0;
 	volatile char           *poll_buf = NULL; 
 	volatile char           *post_buf = NULL;
	volatile char           *poll_buf_f = NULL; 
 	volatile char           *post_buf_f = NULL;
	struct ibv_sge     		list;
	struct ibv_send_wr 		wr;
	struct ibv_send_wr 		*bad_wr = NULL;
	struct ibv_wc 			wc;

	list.addr   = (uintptr_t)ctx->buf;
	list.length = user_param->size;
	list.lkey   = ctx->mr->lkey;

	wr.sg_list             = &list;
	wr.wr.rdma.remote_addr = rem_dest->vaddr;
	wr.wr.rdma.rkey        = rem_dest->rkey;
	wr.wr_id      		   = PINGPONG_READ_WRID;
	wr.num_sge             = MAX_RECV_SGE;
	wr.opcode              = IBV_WR_RDMA_WRITE;
	wr.send_flags          = IBV_SEND_SIGNALED;
	wr.next                = NULL;

	if (user_param->size <= user_param->inline_size)
		wr.send_flags |= IBV_SEND_INLINE;
	
	post_buf = (char*)ctx->buf + user_param->size - 1;
 	poll_buf = (char*)ctx->buf + BUFF_SIZE(ctx->size) + user_param->size - 1;
	if (user_param->calc_first_byte_latency) {
		post_buf_f = (char*)ctx->buf;
		poll_buf_f = (char*)ctx->buf + BUFF_SIZE(ctx->size);
 	}
	/* Done with setup. Start the test. */
	while (scnt < user_param->iters || ccnt < user_param->iters || rcnt < user_param->iters 
					|| (user_param->test_type == DURATION && user_param->state != END_STATE)) {
		if (user_param->state == END_STATE) break;
		//client poll first
		if ((rcnt < user_param->iters || (user_param->test_type == DURATION && user_param->state != END_STATE)) 
						&& !(scnt < 1 && user_param->machine == SERVER)) {
			rcnt++;
			if (user_param->calc_first_byte_latency) {
				while (*poll_buf_f != (char)rcnt && user_param->state != END_STATE);
				if (user_param->test_type == ITERATIONS) 
					t_first_stamp[scnt] = get_cycles();
				else if (user_param->state == SAMPLE_STATE) {
					t_first_stamp[sample_scnt % user_param->iters] = get_cycles();
				}
			}
			while (*poll_buf != (char)rcnt && user_param->state != END_STATE);
			
		}

		if (scnt < user_param->iters || (user_param->test_type == DURATION && user_param->state != END_STATE)) {
			
			
			*post_buf = (char)++scnt;
			if (user_param->calc_first_byte_latency) 
				*post_buf_f = (char)scnt;
			if (user_param->test_type == ITERATIONS) {
				tstamp[scnt-1] = get_cycles();
			}else if (user_param->state == SAMPLE_STATE) {
				tstamp[sample_scnt++ % user_param->iters] = get_cycles();
			}

			if (ibv_post_send(ctx->qp[0],&wr,&bad_wr)) {
				fprintf(stderr, "Couldn't post send: scnt=%d\n",scnt);
				return 11;
			}
		}

		if (ccnt < user_param->iters || (user_param->test_type == DURATION && user_param->state != END_STATE)) {	
		
			do {
				ne = ibv_poll_cq(ctx->cq, 1, &wc);
			} while (ne == 0);
// 			printf("after polling \n");
			if(ne > 0) {
				if (wc.status != IBV_WC_SUCCESS) 
					NOTIFY_COMP_ERROR_SEND(wc,scnt,ccnt);
				ccnt++;

			}
			else if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return 12;
			}		
		}
	}
	end_traffic = get_cycles();
	scnt = (user_param->test_type == DURATION) ? sample_scnt : scnt ;
 	print_report(comm, user_param, scnt, (user_param->state == END_STATE && scnt >  user_param->iters) ? ((scnt + 1) % user_param->iters) : 0);
	return 0;

}

/****************************************************************************** 
 *
 ******************************************************************************/
int main(int argc, char *argv[]) {

	int                         i = 0;
	int                        size_min_pow = 1;
	int                        size_max_pow = 23;
	struct report_options       report = {};
	struct pingpong_context     ctx;
	struct pingpong_dest        my_dest,rem_dest;
	struct ibv_device           *ib_dev;
	struct perftest_comm		user_comm;

	/* init default values to user's parameters */
	memset(&ctx,0,sizeof(struct pingpong_context));
	memset(&user_param, 0, sizeof(struct perftest_parameters));
	memset(&user_comm,0,sizeof(struct perftest_comm));
	memset(&my_dest,0,sizeof(struct pingpong_dest));
	memset(&rem_dest,0,sizeof(struct pingpong_dest));

	user_param.verb    = WRITE;
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


	//run on all sizes
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
					fprintf(stderr,"Unable to perform rdma_server function\n");
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

		// shaking hands and gather the other side info.
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

			if (pp_connect_ctx(&ctx,my_dest.psn,&rem_dest,&user_param)) {
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

		if (user_param.calc_first_byte_latency) {
			ALLOCATE(t_first_stamp,cycles_t,user_param.iters);
		}

		if (user_param.test_type == DURATION) {
			user_param.state = START_STATE;
			signal(SIGALRM, catch_alarm);
			alarm(user_param.margin);
		}
		start_traffic = get_cycles();

		if(run_iter(&user_comm,&ctx,&user_param,&rem_dest))
				return 17;
		if (ctx_close_connection(&user_comm,&my_dest,&rem_dest)) {
			fprintf(stderr," Failed to close connection between server and client\n");
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
		free(tstamp);
		if (user_param.calc_first_byte_latency)
			free(t_first_stamp);
		i++;
		
	}while ((i >= size_min_pow) && (i <= size_max_pow) && (user_param.all == ON));
	printf(RESULT_LINE);
	return 0;

}

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <malloc.h>
#include <getopt.h>
#include <limits.h>
#include <errno.h>
#include "perftest_resources.h"

/****************************************************************************** 
 * Begining
 ******************************************************************************/
int check_add_port(char **service,int port,
				   const char *servername,
				   struct addrinfo *hints,
				   struct addrinfo **res) {

	int number;

	if (asprintf(service,"%d", port) < 0)
		return FAILURE;

	number = getaddrinfo(servername,*service,hints,res);

	if (number < 0) {
		fprintf(stderr, "%s for %s:%d\n", gai_strerror(number), servername, port);
		return FAILURE;
	}

	return SUCCESS;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int create_rdma_resources(struct pingpong_context *ctx,
						  struct perftest_parameters *user_param) { 

	enum rdma_port_space port_space;

	ctx->cm_channel = rdma_create_event_channel();
	if (ctx->cm_channel == NULL) {
		fprintf(stderr, " rdma_create_event_channel failed\n");
		return FAILURE;
	}

	switch (user_param->connection_type) {

		case RC: port_space = RDMA_PS_TCP; break;
		case UD: port_space = RDMA_PS_UDP; break;
		default: port_space = RDMA_PS_TCP;
	}

	if (user_param->machine == CLIENT) {

		if (rdma_create_id(ctx->cm_channel,&ctx->cm_id,NULL,RDMA_PS_TCP)) {
			fprintf(stderr,"rdma_create_id failed\n");
			return FAILURE;
		}

	} else {

		if (rdma_create_id(ctx->cm_channel,&ctx->cm_id_control,NULL,RDMA_PS_TCP)) {
			fprintf(stderr,"rdma_create_id failed\n");
			return FAILURE;
		}

	}

	return SUCCESS;
}

/****************************************************************************** 
 *
 ******************************************************************************/
struct ibv_device* ctx_find_dev(const char *ib_devname) {

	int num_of_device;
	struct ibv_device **dev_list;
	struct ibv_device *ib_dev = NULL;

	dev_list = ibv_get_device_list(&num_of_device);

	if (num_of_device <= 0) {
		fprintf(stderr," Did not detect devices \n");
		fprintf(stderr," If device exists, check if driver is up\n");
		return NULL;
	}

	if (!ib_devname) {
		ib_dev = dev_list[0];
		if (!ib_dev)
			fprintf(stderr, "No IB devices found\n");
	} else {
		for (; (ib_dev = *dev_list); ++dev_list)
			if (!strcmp(ibv_get_device_name(ib_dev), ib_devname))
				break;
		if (!ib_dev)
			fprintf(stderr, "IB device %s not found\n", ib_devname);
	}
	return ib_dev;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int destroy_ctx(struct pingpong_context *ctx,
				struct perftest_parameters *user_parm, struct mcast_parameters    *mcg_params, bool close_device)  {

	int i;
	int test_result = 0;

// 	if (user_parm->use_mcg || user_parm->connection_type == RawEth) {
// 		if (destroy_mcast_group(ctx,user_parm,mcg_params)) {
// 			fprintf(stderr, "failed to destroy MultiCast resources\n");
// 			test_result = 1;
// 		}
// 	}

	if (ctx->ah) {
		if (ibv_destroy_ah(ctx->ah)) {
			fprintf(stderr, "failed to destroy AH\n");
			test_result = 1;
		}
	}
	for (i = 0; i < user_parm->num_of_qps; i++) {
		if (ibv_destroy_qp(ctx->qp[i])) {
			fprintf(stderr, "failed to destroy QP\n");
			test_result = 1;
		}
	}

	if (ibv_destroy_cq(ctx->cq)) {
		fprintf(stderr, "failed to destroy CQ\n");
		test_result = 1;
	}

	if (ibv_dereg_mr(ctx->mr)) {
		fprintf(stderr, "failed to deregister MR\n");
		test_result = 1;
	}

	if (ibv_dealloc_pd(ctx->pd)) {
		fprintf(stderr, "failed to deallocate PD\n");
		test_result = 1;
	}


	if (ctx->channel) {
		if (ibv_destroy_comp_channel(ctx->channel)) {
			test_result = 1;
		}
	}

	if (user_parm->work_rdma_cm || user_parm->use_rdma_cm) {
		rdma_destroy_id(ctx->cm_id);
		rdma_destroy_event_channel(ctx->cm_channel);
	}

	if (close_device && !(user_parm->work_rdma_cm || user_parm->use_rdma_cm)) {
		if (ibv_close_device(ctx->context)) {
			fprintf(stderr, "failed to close device context\n");
			test_result = 1;
		}
	}

// 	if (user_parm->work_rdma_cm == OFF) {
// 		
// 		if (close_device && ibv_close_device(ctx->context)) {
// 			fprintf(stderr, "failed to close device context\n");
// 			test_result = 1;
// 		}
// 
// 	} else {
// 
// 		rdma_destroy_id(ctx->cm_id);
// 		rdma_destroy_event_channel(ctx->cm_channel);
// 		printf("id destroyed\n");
// 		printf("cm_channel destroyed\n\n");
// 
// 
// 	}

	free(ctx->buf);
	free(ctx->qp);
	free(ctx->scnt);
	free(ctx->ccnt);
	return test_result;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int ctx_init(struct pingpong_context *ctx,struct perftest_parameters *user_param) {

	int i,flags;
	uint64_t buff_size;

	ALLOCATE(ctx->qp,struct ibv_qp*,user_param->num_of_qps);
	ALLOCATE(ctx->scnt,int,user_param->num_of_qps);
	ALLOCATE(ctx->ccnt,int,user_param->num_of_qps);

	memset(ctx->scnt, 0, user_param->num_of_qps * sizeof (int));
	memset(ctx->ccnt, 0, user_param->num_of_qps * sizeof (int));

	flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE;
	// ctx->size = SIZE(user_param->connection_type,user_param->size,!(int)user_param->machine);
	ctx->size = user_param->size;
	buff_size = BUFF_SIZE(ctx->size) * 2 * user_param->num_of_qps;
	
	// Allocating the buffer in BUFF_SIZE size to support max performance.
	ctx->buf = memalign(sysconf(_SC_PAGESIZE),buff_size);
	if (!ctx->buf) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		return FAILURE;
	}
	memset(ctx->buf, 0,buff_size);

	// Allocating an event channel if requested.
	if (user_param->use_event) {
		ctx->channel = ibv_create_comp_channel(ctx->context);
		if (!ctx->channel) {
			fprintf(stderr, "Couldn't create completion channel\n");
			return FAILURE;
		}
	}
	// Allocating the Protection domain.
	ctx->pd = ibv_alloc_pd(ctx->context);
	if (!ctx->pd) {
		fprintf(stderr, "Couldn't allocate PD\n");
		return FAILURE;
	}
	if (user_param->verb == READ){
		flags |= IBV_ACCESS_REMOTE_READ;
	}
	// Alocating Memory region and assiging our buffer to it.
	ctx->mr = ibv_reg_mr(ctx->pd,ctx->buf,buff_size,flags);
	if (!ctx->mr) {
		fprintf(stderr, "Couldn't allocate MR\n");
		return FAILURE;
	}

	// Creates the CQ according to ctx_cq_create in perfetst_resources.
	ctx->cq = ibv_create_cq(ctx->context,user_param->cq_size,NULL,ctx->channel,0);
	if (!ctx->cq) {
		fprintf(stderr, "Couldn't create CQ\n");
		return FAILURE;
	}
	for (i=0; i < user_param->num_of_qps; i++) {

		ctx->qp[i] = ctx_qp_create(ctx,user_param);
		if (ctx->qp[i] == NULL) {
			fprintf(stderr," Unable to create QP.\n");
			return FAILURE;
		}

		if (user_param->work_rdma_cm == OFF) {

			if (ctx_modify_qp_to_init(ctx->qp[i],user_param)) {
				fprintf(stderr, "Failed to modify QP to INIT\n");
				return FAILURE;
			}
		}
	}

	return SUCCESS;
}

/****************************************************************************** 
 *
 ******************************************************************************/
struct ibv_qp* ctx_qp_create(struct pingpong_context *ctx,
							 struct perftest_parameters *user_param) {

	struct ibv_qp_init_attr attr;
	struct ibv_qp* qp = NULL;

	memset(&attr, 0, sizeof(struct ibv_qp_init_attr));
	attr.send_cq = ctx->cq;
	attr.recv_cq = ctx->cq; 
	attr.cap.max_send_wr  = user_param->tx_depth;
	attr.cap.max_recv_wr  = user_param->rx_depth;
	attr.cap.max_send_sge = MAX_SEND_SGE;
	attr.cap.max_recv_sge = MAX_RECV_SGE;
	attr.cap.max_inline_data = user_param->inline_size;

	switch (user_param->connection_type) {
		
		case RC : attr.qp_type = IBV_QPT_RC; break;
		case UC : attr.qp_type = IBV_QPT_UC; break;
		case UD : attr.qp_type = IBV_QPT_UD; break;
		case RawEth : attr.qp_type = IBV_QPT_RAW_PACKET; break;
		default:  fprintf(stderr, "Unknown connection type \n");
			return NULL;
	}

	if (user_param->work_rdma_cm) {

		if (rdma_create_qp(ctx->cm_id,ctx->pd,&attr)) {
			fprintf(stderr, " Couldn't create rdma QP - %s\n",strerror(errno));
			return NULL;
		}

		qp = ctx->cm_id->qp;

	} else {

		qp = ibv_create_qp(ctx->pd,&attr);
	}
	return qp;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int ctx_modify_qp_to_init(struct ibv_qp *qp,struct perftest_parameters *user_param)  {

	struct ibv_qp_attr attr;
	int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT;

	memset(&attr, 0, sizeof(struct ibv_qp_attr));
	attr.qp_state        = IBV_QPS_INIT;
	attr.pkey_index      = 0;
	attr.port_num        = user_param->ib_port;

	
	if (user_param->connection_type == RawEth) {
		flags = IBV_QP_STATE | IBV_QP_PORT;
	} else {
		if (user_param->connection_type == UD) {
			attr.qkey = DEFF_QKEY;
			flags |= IBV_QP_QKEY;

		} else {
			switch (user_param->verb) {
				case READ  : attr.qp_access_flags = IBV_ACCESS_REMOTE_READ;  break;
				case WRITE : attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE; break;
				case SEND  : attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE |
												IBV_ACCESS_LOCAL_WRITE;
			}
		flags |= IBV_QP_ACCESS_FLAGS;
		}
	}

	if (ibv_modify_qp(qp,&attr,flags)) {
		fprintf(stderr, "Failed to modify QP to INIT\n");
		return 1;
	}
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
uint16_t ctx_get_local_lid(struct ibv_context *context,int port) {

	struct ibv_port_attr attr;

	if (ibv_query_port(context,port,&attr))
		return 0;

	return attr.lid;
}

/****************************************************************************** 
 *
 ******************************************************************************/
inline int ctx_notify_events(struct ibv_cq *cq,struct ibv_comp_channel *channel) {

	struct ibv_cq       *ev_cq;
	void                *ev_ctx;

	if (ibv_get_cq_event(channel,&ev_cq,&ev_ctx)) {
		fprintf(stderr, "Failed to get cq_event\n");
		return 1;
	}

	if (ev_cq != cq) {
		fprintf(stderr, "CQ event for unknown CQ %p\n", ev_cq);
		return 1;
	}

	ibv_ack_cq_events(cq,1);

	if (ibv_req_notify_cq(cq, 0)) {
		fprintf(stderr, "Couldn't request CQ notification\n");
		return 1;
	}
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
inline void increase_rem_addr(struct ibv_send_wr *wr,int size,
							  int scnt,uint64_t prim_addr) {

	wr->wr.rdma.remote_addr += INC(size);           

	if ( ((scnt+1) % (CYCLE_BUFFER/ INC(size))) == 0)
		wr->wr.rdma.remote_addr = prim_addr;
}

/****************************************************************************** 
 *
 ******************************************************************************/
inline void increase_loc_addr(struct ibv_sge *sg,int size,int rcnt,
							  uint64_t prim_addr,int server_is_ud) {


	//if (server_is_ud)
		//sg->addr -= (CACHE_LINE_SIZE - UD_ADDITION);

	sg->addr  += INC(size);

	if ( ((rcnt+1) % (CYCLE_BUFFER/ INC(size))) == 0 )
		sg->addr = prim_addr;

	//if (server_is_ud)
		//sg->addr += (CACHE_LINE_SIZE - UD_ADDITION);
}

/****************************************************************************** 
 *
 ******************************************************************************/
void mac_from_gid(uint8_t   *mac, uint8_t *gid ){
	
	//manipolation on the gid num to find the mac 
	memcpy(mac, gid + 8, 3);
	memcpy(mac + 3, gid + 13, 3);
	mac[0] ^= 2;
}
/****************************************************************************** 
 *
 ******************************************************************************/
void gen_eth_header(struct ETH_header* eth_header,uint8_t* src_mac,uint8_t* dst_mac, uint16_t eth_type) {
	//prepare ETH packet header
	memcpy(eth_header->src_mac, src_mac, 6);
	memcpy(eth_header->dst_mac, dst_mac, 6);
	eth_header->eth_type = htons(eth_type);
/*	memset(eth_header->dst_mac,1,6);*/
}
/****************************************************************************** 
 *
 ******************************************************************************/
void dump_buffer(unsigned char *bufptr, int len) {
	int i;

	for (i = 0; i < len; i++) {
		printf("%02x:", *bufptr);
		bufptr++;
	}
	printf("\n");
}

/****************************************************************************** 
 *
 ******************************************************************************/
int destroy_mcast_group(struct pingpong_context *ctx,
							   struct perftest_parameters *user_parm,
						       struct mcast_parameters *mcg_params) {
	int i;
	for (i = 0; i < user_parm->num_of_qps; i++) {
		if (ibv_detach_mcast(ctx->qp[i],&mcg_params->mgid,mcg_params->mlid)) {
			fprintf(stderr, "Couldn't deattach QP from MultiCast group\n");
			return 1;
		}
	}
	if (!strcmp(link_layer_str(user_parm->link_type),"IB")) {
		// Removal Request for Mcast group in SM.
		if (join_multicast_group(SUBN_ADM_METHOD_DELETE,mcg_params)) {
			fprintf(stderr,"Couldn't Unregister the Mcast group on the SM\n");
			return 1;
		}
	}

	mcg_params->mcast_state &= ~MCAST_IS_ATTACHED;
	return 0;
}
/******************************************************************************
 *
 ******************************************************************************/
void catch_alarm(int sig) {
	switch (user_param.state) {
		case START_STATE:
			user_param.state = SAMPLE_STATE;
			start_sample = get_cycles();
			alarm(user_param.duration - 2*(user_param.margin));
			break;
		case SAMPLE_STATE:
			user_param.state = STOP_SAMPLE_STATE;
			end_sample = get_cycles();
			alarm(user_param.margin);
			break;
		case STOP_SAMPLE_STATE:
			user_param.state = END_STATE;
			break;
		default:
			printf("unknown state\n");
	}
}

/****************************************************************************** 
 * End
 ******************************************************************************/



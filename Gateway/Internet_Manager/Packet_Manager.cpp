#include <netinet/ip.h>
#include <arpa/inet.h>
#include <linux/udp.h>
#include "Packet_Manager.hpp"
#include "../Tables/Nonaitf_Dests_Table.hpp"
#include "../Flow.hpp"
#include "../Constants.hpp"
#include "../Hasher.hpp"
#include "../logger.hpp"
#include "../Helpers.hpp"

Packet_Sniffer* Packet_Manager::listener = NULL;
Packet_Manager::Packet_Manager(Packet_Sniffer* listener_in){
	log(logINFO) << "Creating Packet_Manager";
	listener = listener_in;

	my_netfilterqueue_handle = nfq_open();
	nfq_unbind_pf(my_netfilterqueue_handle, AF_INET);
	nfq_bind_pf(my_netfilterqueue_handle, AF_INET);

	//Setup the state and queue
	state.store(STARTING, boost::memory_order_relaxed);

	internet_queue_handle = nfq_create_queue(my_netfilterqueue_handle, 1, &(Packet_Manager::packet_callback), NULL);
	nfq_set_mode(internet_queue_handle, NFQNL_COPY_PACKET, 0xffff);

	state.store(STARTED, boost::memory_order_relaxed);
}

void Packet_Manager::start(){

	log(logINFO) << "Starting Packet_Manager";
	int fd;
	int rv;
	char buf[4096] __attribute__ ((aligned));

	fd_set readset;
	struct timeval tv;

	fd = nfq_fd(my_netfilterqueue_handle);

	while (ENDING != state.load(boost::memory_order_relaxed)) {
		FD_ZERO(&readset);
		FD_SET(fd, &readset);
		tv.tv_sec = 0;
		tv.tv_usec = 500;
		if (select(fd+1, &readset, NULL, NULL, &tv) <= 0 ) {
			continue;
		}

		rv = recv(fd, buf, sizeof(buf), 0);//MSG_DONTWAIT );
		if (rv < 0) {
			continue;
		}
		log(logDEBUG) << "Internet Queue is Receiceiving Packet";
		nfq_handle_packet(my_netfilterqueue_handle, buf, rv);
	}
}

void Packet_Manager::stop(){
	state.store(ENDING, boost::memory_order_relaxed);
	nfq_destroy_queue( internet_queue_handle );
	nfq_close(my_netfilterqueue_handle);
}

static unsigned short compute_checksum(unsigned short *addr, unsigned int count) {
	register unsigned long sum = 0;
	while (count > 1) {
		sum += * addr++;
		count -= 2;
	}
	//if any bytes left, pad the bytes and add
	if(count > 0) {
		sum += ((*addr)&htons(0xFF00));
	}
	//Fold sum to 16 bits: add carrier to result
	while (sum>>16) {
		sum = (sum & 0xffff) + (sum >> 16);
	}
	//one's complement
	sum = ~sum;
	return ((unsigned short)sum);
}

/* set ip checksum of a given ip header*/
void compute_ip_checksum(struct iphdr* iphdrp){
	iphdrp->check = 0;
	iphdrp->check = compute_checksum((unsigned short*)iphdrp, iphdrp->ihl<<2);
}


int Packet_Manager::packet_callback(struct nfq_q_handle *qh, struct nfgenmsg *nfmsg, struct nfq_data *nfad, void *data){
	log(logINFO) << "----------------INTERCEPTED PACKET----------------";

	struct nfqnl_msg_packet_hdr *ph;
	int len;
	unsigned char *ORIGINAL_DATA;
	struct iphdr *ipHeader;

	//get the packet id
	ph = nfq_get_msg_packet_hdr(nfad);
	u_int32_t id = ntohl(ph->packet_id);

	//get the packet contents
	len = nfq_get_payload(nfad, &ORIGINAL_DATA);


	log(logDEBUG) << "packet len: " << len;
	ipHeader = (struct iphdr *)ORIGINAL_DATA;

	//DEBUG CHECKS
	log(logDEBUG2) << std::hex << ipHeader->check;


	if(ipHeader->protocol == 143){
		//if this gateway is the destination, remove the AITF header and allow packet
		if(ipHeader->daddr == MY_IP){
			log(logDEBUG) << "Destination is this gateway";
			unsigned char modified_packet[len-82];
			memcpy(&modified_packet[0], ORIGINAL_DATA, sizeof(*ipHeader));
			memcpy(&modified_packet[sizeof(*ipHeader)], ORIGINAL_DATA + sizeof(*ipHeader) + 82, len - sizeof(*ipHeader) - 82);
			((struct iphdr*) &modified_packet[0])->protocol = *((uint8_t*)ORIGINAL_DATA+sizeof(*ipHeader));
			((struct iphdr*) &modified_packet[0])->tot_len = htons(len-82);

			//compute the new checksum
			compute_ip_checksum((struct iphdr*) &modified_packet[0]);
			log(logDEBUG2) << std::hex << ((struct iphdr*) &modified_packet[0])->check;
			return nfq_set_verdict(qh, id, NF_ACCEPT, len-82, &modified_packet[0]);
		}

		//If the packet already has an RR header
		log(logINFO) << "AITF PACKET!!!!";


		//make a copy of the packet to edit
		unsigned char modified_packet[len];
		memcpy(&modified_packet[0], ORIGINAL_DATA, len);

		//get the Flow from the RR header
		Flow flow(std::vector<uint8_t>(&modified_packet[sizeof(*ipHeader)+1], &modified_packet[sizeof(*ipHeader)+1]+81));
		uint8_t ptr = flow.pointer + 1;
		log(logDEBUG2) << "src_ip: " << Helpers::ip_to_string(flow.src_ip);
		log(logDEBUG2) << "dst_ip: " << Helpers::ip_to_string(flow.dst_ip);

		//if the number of gateways exceeds 6 drop the packet
		if(ptr > 5){
			log(logDEBUG) << "Dropping packet due to excessive gateways";
			return nfq_set_verdict(qh, id, NF_DROP, 0, NULL);
		}

		//compute the rvalue and set the gateway info
		uint64_t rvalue = Hasher::hash(*gateway_key, (unsigned char*) &flow.dst_ip, 4);
		flow.set_gtw_ip_at(ptr, MY_IP);
		flow.set_gtw_rvalue_at(ptr, rvalue);
		flow.pointer = ptr;


		//grab the payload if the original protocol was UDP
		std::vector<uint8_t> payload(0);
		if(modified_packet[sizeof(*ipHeader)] == IPPROTO_UDP){
			struct udphdr* udp_info = (struct udphdr*) ORIGINAL_DATA + sizeof(*ipHeader) + 82;
			unsigned char * data_start = ORIGINAL_DATA + sizeof(*ipHeader) + 82 + sizeof(*udp_info);
			int data_len = len - sizeof(*ipHeader) - 82 - sizeof(*udp_info);
			payload.assign(data_start, data_start+data_len);
		}
		//check to see if there is a filter for the flow
		bool is_allowed = listener->is_allowed(flow, payload);
		
		for(int i = 0; i <= flow.pointer; i++){
			log(logDEBUG2) << "gtw ip " << i << Helpers::ip_to_string(flow.get_gtw_ip_at(i));
			log(logDEBUG2) << "gtw rvalue " << i << flow.get_gtw_rvalue_at(i);
		}
		if(!is_allowed){
			return nfq_set_verdict(qh, id, NF_DROP, 0, NULL);
		}


		//if the packet is allowed on then reinsert the new flow and recompute checksum
		memcpy(&modified_packet[sizeof(*ipHeader) + 1], &flow.to_byte_vector()[0], 81);
		//compute the new checksum
		compute_ip_checksum((struct iphdr*) &modified_packet[0]);
		log(logDEBUG2) << std::hex << ((struct iphdr*) &modified_packet[0])->check;
		return nfq_set_verdict(qh, id, NF_ACCEPT, len, &modified_packet[0]);
	}
	else{
		//if it isnt AITF traffic
		log(logINFO) << "Non AITF traffic";
		//pull out the destination ip
		u_int32_t dst_ip = ipHeader->daddr;
		//If the destination is not AITF compliant
		if(nonaitf_dests_table->is_nonaitf(dst_ip) || dst_ip == MY_IP){
			log(logDEBUG) << "Destination is Non AITF OR is this gateway";
			//the destination is not aitf enabled. allow the packet to continue
			return nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);
		}
		else{
			log(logDEBUG) << "Destination is AITF dst: " << Helpers::ip_to_string(dst_ip);
			//The dest is AITF enabled
			//Insert a new RR header
			unsigned char modified_packet[len+82];
			memcpy(&modified_packet[0], ORIGINAL_DATA, sizeof(*ipHeader));
			memcpy(&modified_packet[sizeof(*ipHeader) + 82], ORIGINAL_DATA + sizeof(*ipHeader), len - sizeof(*ipHeader));

			//set the protocol and new length
			((struct iphdr*) &modified_packet[0])->protocol = 143;
			((struct iphdr*) &modified_packet[0])->tot_len = htons(len+82);

			log(logDEBUG) << "Creating flow";
			//create the new flow for the RR header
			Flow flow;
			flow.src_ip = ipHeader->saddr;
			flow.dst_ip = ipHeader->daddr;
			flow.pointer = 0;
			flow.gtw0_ip = MY_IP;
			flow.gtw0_rvalue = Hasher::hash(*gateway_key, (unsigned char*) &flow.dst_ip, 4);
#ifdef FORGE
			flow.gtw0_rvalue = 1;
#endif

			log(logDEBUG) << "Getting UDP Info";
			std::vector<uint8_t> payload(0);
			//get the payload if the packet type is UDP as it may be a handshake
			if(ipHeader->protocol == IPPROTO_UDP){
				struct udphdr* udp_info = (struct udphdr*) (ORIGINAL_DATA + sizeof(*ipHeader));
				uint8_t* data_start = ORIGINAL_DATA + sizeof(*ipHeader) + sizeof(*udp_info);
				int data_len = len - sizeof(*ipHeader) - sizeof(*udp_info);
				payload.assign(data_start, data_start + data_len);
			}

			//Check for any filters for this flow
			bool is_allowed = listener->is_allowed(flow, payload);

			//if it is filtered then deny the packet
			if(!is_allowed){
				return nfq_set_verdict(qh, id, NF_DROP, 0, NULL);
			};

			//if it is allowed on then insert the RR header into the modified packet
			modified_packet[sizeof(*ipHeader)] = ipHeader->protocol;
			memcpy(&modified_packet[sizeof(*ipHeader)+1], &flow.to_byte_vector()[0], 81);

			//compute the new checksum
			compute_ip_checksum((struct iphdr*) &modified_packet[0]);
			return nfq_set_verdict(qh, id, NF_ACCEPT, len+82, &modified_packet[0]);

		}

	}
	/*

	// removes the AITF header
	unsigned char modified_packet[len-82];
	memcpy(&modified_packet[0], ORIGINAL_DATA, sizeof(*ipHeader));
	memcpy(&modified_packet[sizeof(*ipHeader)], ORIGINAL_DATA + sizeof(*ipHeader) + 82, len - sizeof(*ipHeader) - 82);
	((struct iphdr*) &modified_packet[0])->protocol = 17;
	((struct iphdr*) &modified_packet[0])->tot_len = htons(len-82);

	//compute the new checksum
	compute_ip_checksum((struct iphdr*) &modified_packet[0]);
	log(logDEBUG2) << std::hex << ((struct iphdr*) &modified_packet[0])->check;

	if(ipHeader->protocol == IPPROTO_UDP){
	log(logDEBUG) << "Udp Packet";

	unsigned char modified_packet[len+82];
	memcpy(&modified_packet[0], ORIGINAL_DATA, sizeof(*ipHeader));
	memcpy(&modified_packet[sizeof(*ipHeader) + 82], ORIGINAL_DATA + sizeof(*ipHeader), len - sizeof(*ipHeader));
	((struct iphdr*) &modified_packet[0])->protocol = 143;
	((struct iphdr*) &modified_packet[0])->tot_len = htons(len+82);

//compute the new checksum
compute_ip_checksum((struct iphdr*) &modified_packet[0]);
log(logDEBUG2) << std::hex << ((struct iphdr*) &modified_packet[0])->check;
return nfq_set_verdict(qh, id, NF_ACCEPT, len+82, &modified_packet[0]);
}*/

//issue a verdict on a packet
//qh: netfilter queue handle; id: ID assigned to packet by netfilter; verdict: verdict to return to netfilter, data_len: number
//of bytes of data pointed by buf, buf: the buffer that contains the packet data (payload)
// return nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);
//return nfq_set_verdict(qh, id, NF_ACCEPT, len, ORIGINAL_DATA);
}




Packet_Manager::~Packet_Manager(void) {
	log(logINFO) << "Ending Packet_Manager";
	this->state.store(this->ENDING, boost::memory_order_relaxed);
	log(logINFO) << "Ended Packet_Manager";
}

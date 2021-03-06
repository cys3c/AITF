/*
 * HostMain.cpp
 *
 */

#include <string>

#include <boost/thread.hpp>

#include "logger.hpp"

#include "Managers/PacketManager.hpp"
#include "Modules/FilterModule.hpp"
#include "Modules/PolicyModule.hpp"
#include "Servers/Udp_Server.hpp"

loglevel_e loglevel = logERROR;
uint8_t ESCELATION = 0 ;

void set_log_level(int level){
	switch(level){
		case 0: loglevel = logERROR;
			break;
		case 1: loglevel = logWARNING;
			break;
		case 2: loglevel = logINFO;
			break;
		case 3: loglevel = logDEBUG;
			break;
		case 4: loglevel = logDEBUG2;
			break;
	}
}

void start_server(Udp_Server * udps) {
	udps->start();
}

int main(int argc, char **argv){


	char * cur_gateway;
	int gateway_offset = 0;
	ESCELATION = atoi(argv[1]);

	set_log_level(atoi(argv[2]));

	if (argc < 6) {
		llog(logERROR) << "Minimum parameters not meet\n USAGE : ./Hosts.out -g Gateway Host_IP [Host_IP .. ] [-g Gateway Host_IP [Host_IP .. ]]";
		return -1;
	} else if (std::string(argv[3]) == "-g" || std::string(argv[3]) == "-G") {
		cur_gateway = argv[4];
		gateway_offset += 2;
	} else{
		llog(logERROR) << "Gateway not detected\n USAGE : ./Hosts.out -g Gateway Host_IP [Host_IP .. ] [-g Gateway Host_IP [Host_IP .. ]]";
		return -1;

	}

	int i;
	char command[200];
	PacketManager * pms[argc-5];
	char * host_ip[argc-5];

	int counter = 0, vic_q, atk_q;

	PolicyModule * policy = new PolicyModule();
	FilterModule * filter = new FilterModule();

	llog(logINFO) << "Starting Main";

	Udp_Server * udps;
	udps = udps->getInstance();

	for ( i = 5; i < argc; i ++ ) {

		if (std::string(argv[i]) == "-g" || std::string(argv[i]) == "-G") {
				cur_gateway = argv[i+1];
				gateway_offset += 2;
				i++;
		} else {
			vic_q = (counter*2+3);
			atk_q = (counter*2+4);
			host_ip[counter] = argv[i];

			sprintf( command, "iptables -A INPUT -d %s -s 10.4.13.0/24 -j NFQUEUE --queue-num %u",host_ip[counter], vic_q );
			system( command );
			sprintf( command, "iptables -A OUTPUT -s %s -d 10.4.13.0/24 -j NFQUEUE --queue-num %u", host_ip[counter], atk_q );
			system( command );
			llog(logINFO) << "---------------------------------------------------------Test here";
			pms[counter] = new PacketManager( inet_addr(host_ip[counter]), inet_addr(cur_gateway) , vic_q, atk_q, policy, filter);
			counter ++;
		}

	}
	boost::thread udp_thread(&start_server, udps);

	int x;
	std::cin >> x;

	udps->stop();
	udp_thread.join();

	for ( i = counter - 1 ; i >= 0; i -- ) {
		vic_q = (i*2+3);
		atk_q = (i*2+4);

		delete pms[i];

		sprintf( command, "iptables -D OUTPUT -s %s -d 10.4.13.0/24 -j NFQUEUE --queue-num %u", host_ip[i], atk_q );
		system( command );
		sprintf( command, "iptables -D INPUT -d %s -s 10.4.13.0/24 -j NFQUEUE --queue-num %u", host_ip[i], vic_q );
		system( command );
	}

	delete policy;
	delete filter;

	llog(logINFO) << "Finishing Main";

}






#include <boost/bind.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include "Async_Auto_Table.hpp"
#include "../logger.hpp"

Async_Auto_Table::Async_Auto_Table() {
	print_timer = NULL;
}

Async_Auto_Table::Async_Auto_Table(std::string filename, uint32_t timeout) {

	std::ofstream filehandle;
	filehandle.open(filename);

	print_timer = new boost::asio::deadline_timer(table_io, boost::posix_time::milliseconds(timeout));
	print_timer->async_wait(boost::bind(&Async_Auto_Table::printStatus, this, boost::asio::placeholders::error, filehandle, timeout));
}

Async_Auto_Table::~Async_Auto_Table() {

	if (print_timer) {
		print_timer->cancel();
		delete(print_timer);
	}

}

int Async_Auto_Table::getValue(uint32_t ip) {
	int value = -1;
	table_mutex.lock();
	if (table.count(ip) > 0) {
		value = table[ip];
	}
	table_mutex.unlock();
	return value;
}

int Async_Auto_Table::compareValue(uint32_t ip, int value) {
	int ret = 0;
	table_mutex.lock();
	if (table.count(ip) < 1) {
		ret = -2;
	} else if (table[ip] > value) {
		ret = 1;
	} else if (table[ip] < value) {
		ret = -1;
	} else {
		ret = 0;
	}
	table_mutex.unlock();
	return ret;
}

/*
 * addValue: adds the value
 *	input: 	ip - some arbitrary ip address
 *			value - the value to add and decrement
 *			max - some value to compare the new table value to, give a non positive if no comparison should take place
 *			timeout - the time in ms for the decrement to occur
 *	output: the given value or -1 if the max was surpassed with the new value added and not already surpassed
 */
int Async_Auto_Table::addValue(uint32_t ip, int value, int max, uint32_t timeout) {
	int ret = value;
	table_mutex.lock();

	table[ip] += value;

	if ((max > 0) && (table[ip] > max) && ((table[ip] - value) <= max) )
		ret = -1;

	table_mutex.unlock();

	boost::shared_ptr<boost::asio::deadline_timer> timer(new boost::asio::deadline_timer(table_io, boost::posix_time::seconds(1)));
	timer->async_wait(boost::bind(&Async_Auto_Table::decrement, this, boost::asio::placeholders::error, timer, ip, value));
	return ret;
}


void Async_Auto_Table::decrement(const boost::system::error_code& e, boost::shared_ptr<boost::asio::deadline_timer> timer, uint32_t ip, int value) {
	timer.reset();

	table_mutex.lock();

	table[ip] -= value;

	if (table[ip] == 0)
		table.erase(ip);

	table_mutex.unlock();
}

void Async_Auto_Table::printStatus(const boost::system::error_code& e, std::ofstream filehandle, uint32_t timeout) {

	if (e == boost::asio::error::operation_aborted) {
		filehandle.close();
		return;
	}

	table_mutex.lock();

	for ( auto iter = table.begin(); iter != table.end(); ++ iter ) {
		filehandle << "," << iter->first << "," << iter->second;
	}

	table_mutex.unlock();

	print_timer->expires_from_now(boost::posix_time::milliseconds(timeout));
	print_timer->async_wait(boost::bind(&Async_Auto_Table::printStatus, this, boost::asio::placeholders::error, filehandle, timeout));

}

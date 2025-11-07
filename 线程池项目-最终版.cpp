#include <iostream>
#include <future>
#include <chrono>
#include <vector>
#include <atomic>
#include "threadpool.h"

using namespace std;

int sum1(int a , int b)
{
	this_thread::sleep_for(std::chrono::seconds(1));
	return a + b;
}

int sum2(int a, int b , int c)
{
	this_thread::sleep_for(std::chrono::seconds(1));
	return a + b + c;
}

void io_thread(int listenfd)
{

}

void worker_thread(int clientfd)
{

}


int main()
{
	ThreadPool pool;
	pool.setMode(PoolMode::MODE_CACHED);
	pool.start(2);

	future <int> r1 = pool.submitTask(sum1, 1, 2);

	cout << r1.get() << endl;
}
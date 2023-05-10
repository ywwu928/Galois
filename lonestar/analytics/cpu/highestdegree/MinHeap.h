// A C++ program to demonstrate common Binary Heap Operations
#include <iostream>
#include <fstream>
#include <sstream>
#include <climits>
// using namespace std;

// Prototype of a utility function to swap two uint32_tegers
void swap(uint64_t *x, uint64_t *y);

// A class for Min Heap
class MinHeap
{
	uint64_t *count_arr; // pouint32_ter to array of counts in heap
	uint64_t *id_arr; // pouint32_ter to array of ids in heap
	int capacity; // maximum possible size of min heap
	int heap_size; // Current number of elements in min heap
public:
	// Constructor
	MinHeap(int capacity);

	// to heapify a subtree with the root at given index
	void MinHeapify(int i);

	int size() { return heap_size; }
	
	int parent(int i) { return (i-1)/2; }

	// to get index of left child of node at index i
	int left(int i) { return (2*i + 1); }

	// to get index of right child of node at index i
	int right(int i) { return (2*i + 2); }

	// to extract the root which is the minimum element
	uint64_t extractMin();

	// Decreases key value of key at index i to new_val
	void decreaseKey(int i, uint64_t new_val);

	// Returns the minimum key (key at root) from min heap
	uint64_t getMin() { return count_arr[0]; }

	// Deletes a key stored at index i
	void deleteKey(int i);

	// Inserts a new key 'k'
	void insertKey(uint64_t k, uint64_t id);
	
	// Prints the count of the heap
	void printCount();
	
	// Prints the ID of the heap
	void printId();
	
	// Writes the ID of the heap to a file
	void writeId(std::ofstream& file);
};

// Constructor: Builds a heap from a given array a[] of given size
MinHeap::MinHeap(int cap)
{
	heap_size = 0;
	capacity = cap;
	count_arr = new uint64_t[cap];
	id_arr = new uint64_t[cap];
}

// Inserts a new key 'k'
void MinHeap::insertKey(uint64_t k, uint64_t id)
{
	if (heap_size == capacity)
	{
		std::cout << "\nOverflow: Could not insertKey\n";
		std::cout << k << std::endl;
		return;
	}

	// First insert the new key at the end
	heap_size++;
	int i = heap_size - 1;
	count_arr[i] = k;
	id_arr[i] = id;

	// Fix the min heap property if it is violated
	while (i != 0 && count_arr[parent(i)] > count_arr[i]) {
		swap(&count_arr[i], &count_arr[parent(i)]);
		swap(&id_arr[i], &id_arr[parent(i)]);
		i = parent(i);
	}
}

// Decreases value of key at index 'i' to new_val. It is assumed that
// new_val is smaller than count_arr[i].
void MinHeap::decreaseKey(int i, uint64_t new_val)
{
	count_arr[i] = new_val;
	while (i != 0 && count_arr[parent(i)] > count_arr[i]) {
		swap(&count_arr[i], &count_arr[parent(i)]);
		swap(&id_arr[i], &id_arr[parent(i)]);
		i = parent(i);
	}
}

// Method to remove minimum element (or root) from min heap
uint64_t MinHeap::extractMin()
{
	if (heap_size <= 0)
		return INT_MAX;
	if (heap_size == 1)
	{
		heap_size--;
		return count_arr[0];
	}

	// Store the minimum value, and remove it from heap
	uint64_t root = count_arr[0];
	count_arr[0] = count_arr[heap_size-1];
	id_arr[0] = id_arr[heap_size-1];
	heap_size--;
	MinHeapify(0);

	return root;
}


// This function deletes key at index i. It first reduced value to minus
// infinite, then calls extractMin()
void MinHeap::deleteKey(int i)
{
	decreaseKey(i, INT_MIN);
	extractMin();
}

// A recursive method to heapify a subtree with the root at given index
// This method assumes that the subtrees are already heapified
void MinHeap::MinHeapify(int i)
{
	int l = left(i);
	int r = right(i);
	int smallest = i;
	if (l < heap_size && count_arr[l] < count_arr[i])
		smallest = l;
	if (r < heap_size && count_arr[r] < count_arr[smallest])
		smallest = r;
	if (smallest != i)
	{
		swap(&count_arr[i], &count_arr[smallest]);
		swap(&id_arr[i], &id_arr[smallest]);
		MinHeapify(smallest);
	}
}

// This function prints the count of the heap
void MinHeap::printCount()
{
	std::cout << "Counts of the min heap:" << std::endl;
	for (int i=0; i<heap_size; i++) {
		std::cout << count_arr[i] << std::endl;
	}
}

// This function prints the count of the heap
void MinHeap::printId()
{
	std::cout << "ID of the min heap:" << std::endl;
	for (int i=0; i<heap_size; i++) {
		std::cout << id_arr[i] << std::endl;
	}
}

// This function writes the count of the heap to a file
void MinHeap::writeId(std::ofstream& file)
{
	for (int i=0; i<heap_size; i++) {
		file << id_arr[i] << std::endl;
	}
}

// A utility function to swap two elements
void swap(uint64_t *x, uint64_t *y)
{
	uint64_t temp = *x;
	*x = *y;
	*y = temp;
}


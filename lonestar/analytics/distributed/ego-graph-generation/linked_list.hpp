class Node {
    public:
        uint64_t data;
        Node* next;

        Node() {
            data = 0;
            next = NULL;
        }

        Node(uint64_t data) {
            this->data = data;
            this->next = NULL;
        }
};

class LinkedList {
    Node* head;
    Node* tail;
    Node* victim;
    unsigned int max_size;
    unsigned int curr_size;

    public:
        LinkedList() {
            head = NULL;
            tail = NULL;
            victim = NULL;
            max_size = 10;
            curr_size = 0;
        }
        
        LinkedList(unsigned int size) {
            head = NULL;
            tail = NULL;
            victim = NULL;
            max_size = size;
            curr_size = 0;
        }

        void setMax(unsigned int);
        
        void deleteHead();
        
        void insertNode(uint64_t);

        uint64_t getVictim();
        
        void printList();
        
        void clear();
};

void LinkedList::setMax(unsigned int size) {
    head = NULL;
    tail = NULL;
    max_size = size;
    curr_size = 0;
    return;
}

void LinkedList::deleteHead() {
    if (curr_size == 0) {
        std::cout << "Empty List!" << std::endl;
        return;
    }

    if (curr_size == 1) {
        Node *temp = victim;
        victim = head;
        head = NULL;
        tail = NULL;
        delete temp;
        curr_size--;
        return;
    }

    Node *temp = victim;
    victim = head;
    head = head->next;
    delete temp;
    curr_size--;
    return;
}

void LinkedList::insertNode(uint64_t data) {
    Node* newNode = new Node(data);

    if (curr_size == 0) {
        head = newNode;
        tail = newNode;
        curr_size++;
        return;
    }

    if (curr_size < max_size) {
        tail->next = newNode;
        tail = newNode;
        curr_size++;
        return;
    }

    deleteHead();
    tail->next = newNode;
    tail = newNode;
    curr_size++;
    return;
}

uint64_t LinkedList::getVictim() {
    return victim->data;
}

void LinkedList::printList() {
    Node* temp = head;

    if (curr_size == 0) {
        std::cout << "Empty List!" << std::endl;
        return;
    }

    std::cout << "Linked List:" << std::endl;

    while (temp != NULL) {
        std::cout << temp->data << std::endl;
        temp = temp->next;
    }
}

void LinkedList::clear() {
    head = NULL;
    tail = NULL;
    curr_size = 0;
}

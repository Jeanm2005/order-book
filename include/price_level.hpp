#pragma once
#include "order.hpp"

struct PriceLevel {
    Price price = 0;
    Quantity total_qty = 0;
    Order* head = nullptr;
    Order* tail = nullptr;

    bool empty() const { return head == nullptr; }

    void push_back(Order* o) {
        o->prev = tail;
        o->next = nullptr;
        if (tail) {
            tail->next = o;
        } else {
            head = o;
        }
        tail = o;
        total_qty += o->quantity;
    }

    void remove(Order* o) {
        total_qty -= o->quantity;
        if (o->prev) {
            o->prev->next = o->next;
        } else {
            head = o->next;
        }
        if (o->next) {
            o->next->prev = o->prev;
        } else {
            tail = o->prev;
        }
        o->prev = o->next = nullptr;
    }

    void reduce_head(Quantity filled) {
        head->quantity -= filled;
        total_qty -= filled;
    }
};
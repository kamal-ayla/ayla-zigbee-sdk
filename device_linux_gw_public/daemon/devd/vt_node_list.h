#ifndef __VT_NODE_LIST_H__
#define __VT_NODE_LIST_H__


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <ayla/log.h>

typedef struct node
{
	char data[24];
	struct node *link;
}LinkList;

struct node* AllocateNode( void );
void FreeNode( LinkList *FreeNode );
void DisplayNode( void );
void InsertNode ( const char* data );
void RemoveNode ( const char* data );
bool findNode ( const char* data );

#endif /* __VT_NODE_LIST_H__ */

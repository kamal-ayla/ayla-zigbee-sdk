#include "vt_node_list.h"

LinkList *Head = NULL;


/*{{{AllocateNode()*/
struct node* AllocateNode( void )
{
	LinkList *AllocateData= NULL;
	
	AllocateData = (LinkList *)malloc(sizeof(LinkList));
	if (NULL !=AllocateData)
	{
		return AllocateData;
	}
	return NULL;
}
/*}}}*/

/*{{{FreeNode()*/
void FreeNode( LinkList *FreeNode )
{
	if (NULL !=FreeNode)
	{
		free (FreeNode);
		FreeNode = NULL;
	}
}
/*}}}*/


/*{{{DisplayNode()*/
void DisplayNode( void )
{
	if ( NULL == Head )
	{
		log_info("\n vt_node_info list empty \n");
	}
	else
	{
		LinkList *p = NULL;
		
		p=Head;
		log_info("\nvt_mode_info list \n");
		while (NULL!=p)
		{
			log_info("%s ",p->data);
			p=p->link;
		}
		log_info("\n");
	}	
}
/*}}}*/

/*{{{InsertNodeAtLast()*/
void InsertNode ( const char* data )
{
	LinkList *p = NULL;
	LinkList *Node = NULL;
	
	log_info("InsertNode called");
	
	if ( NULL == Head)
	{
		Node = AllocateNode();
		if ( NULL != Node )
		{
			strcpy(Node->data, data);
			Node->link = NULL;
			Head = Node;
		}
	}
	else
	{
		p = Head;
		
		while (NULL!=p->link)
		{
			p = p->link;
		}
	
		Node = AllocateNode();
		if ( NULL != Node )
		{
			strcpy(Node->data, data);
			Node->link = NULL;
			p->link = Node;
		}
	}
	DisplayNode();
}
/*}}}*/


/*{{{RemoveNode*/
void RemoveNode ( const char* data )
{
	LinkList *temp = NULL;
	LinkList *prev = NULL;
	log_info("RemoveNode called");
	if (NULL==Head)
	{
		log_info("vt_node_info list is empty");
		return;
	}else{
		temp = Head;

		if (temp != NULL && (0 == strcmp(temp->data,data))) {
			Head = temp->link;
			FreeNode(temp);
			DisplayNode();
			return;
		}
		// Find the key to be deleted
		while (temp != NULL && (0 != strcmp(temp->data,data))) {
			prev = temp;
			temp = temp->link;
		}

		// If the key is not present
		if (temp == NULL) return;

		// Remove the node
		prev->link = temp->link;
		FreeNode(temp);
	}
	DisplayNode();
}
/*}}}*/

/*{{{findNode*/
bool findNode ( const char* data )
{
	LinkList *p = Head;
	bool node_find_flag = 0;
	
	if (NULL != Head)
	{
		while (NULL != p) 
		{
			if(0 == strcmp(p->data,data))
			{
				node_find_flag = 1;
				break;
			}
			p = p->link;
		}
	}
	return node_find_flag;
}
/*}}}*/


#pragma once

// name made up
class CAnimBlendLink
{
public:
	CAnimBlendLink *next;
	CAnimBlendLink *prev;

	void Init(){
		next = nullptr;
		prev = nullptr;
	}
	void Prepend(CAnimBlendLink *link){
		if(next)
		        next->prev = link;
		link->next = next;
		link->prev = this;
		next = link;
	}
	void Remove(){
		if(prev)
			prev->next = next;
		if(next)
			next->prev = prev;
		Init();
	}
};

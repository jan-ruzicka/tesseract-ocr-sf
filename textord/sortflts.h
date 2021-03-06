/**********************************************************************
 * File:        sortflts.h  (Formerly sfloats.h)
 * Description: Code to maintain a sorted list of floats.
 * Author:		Ray Smith
 * Created:		Mon Oct  4 16:15:40 BST 1993
 *
 * (C) Copyright 1993, Hewlett-Packard Ltd.
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 ** http://www.apache.org/licenses/LICENSE-2.0
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 *
 **********************************************************************/

#ifndef           SORTFLTS_H
#define           SORTFLTS_H

#include          "elst.h"
#include          "notdll.h"
#include          "notdll.h"

class SORTED_FLOAT:public ELIST_LINK
{
  friend class SORTED_FLOATS;

  public:
    SORTED_FLOAT() { 
    }                            //empty constructor
    SORTED_FLOAT(              //create one
                 float value,  //value of entry
                 INT32 key) {  //reference
      entry = value;
      address = key;
    }
  private:
    float entry;                 //value of float
    INT32 address;               //key
};

ELISTIZEH (SORTED_FLOAT)
class SORTED_FLOATS
{
  public:
    SORTED_FLOATS() {  //empty constructor
      it.set_to_list (&list);
    }
    void add(              //add sample
             float value,  //sample float
             INT32 key);   //retrieval key
    void remove(             //delete sample
                INT32 key);  //key to delete
    float operator[] (           //index to list
      INT32 index);              //item to get

  private:
    SORTED_FLOAT_LIST list;      //list of floats
    SORTED_FLOAT_IT it;          //iterator built-in
};
#endif

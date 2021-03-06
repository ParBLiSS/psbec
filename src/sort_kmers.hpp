/***
 *
 *  Author: Ankit Shah <shah.29ankit@gmail.com>
 *          Sriram P C <sriram.pc@gmail.com>
 *
 *  Copyright 2011-2012 Ankit Shah, Sriram P C, Srinivas Aluru
 *
 *  This file is part of Parallel Reptile (version 1.1)
 *
 *  PReptile is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  PReptile is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with Libpnorm. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef _SORT_KMERS_H
#define _SORT_KMERS_H

/***
 * Author : Ankit R Shah
 * Uses a parallel MPI sample sort to sort the k-mers
 */

#include "mpi.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <climits>

#include "util.h"
#include "load_balance.hpp"

template <typename StructDataType, typename SizeType>
void eliminate_threshold(StructDataType *&array, SizeType &count,
                         int threshold){
    static SizeType i, j;

    if(count ==0 || threshold == 0)
        return;
    for(j=0,i=0;i<count;i++){
        int cval = (int) (array[i].count);
        if(cval >= threshold) {
            array[j].ID = array[i].ID;
            array[j].count = array[i].count;
            j++;
        }
    }
    count = j;
}

template <typename StructDataType, typename SizeType>
void eliminate_dupes(StructDataType *&array, SizeType &count) {
    static SizeType i, j;
    if(count == 0)
        return;
    for(j = 0,i=1;i<count;i++){
        if(array[i].ID == array[i-1].ID){
            // Add it to the previous ID. Limit count to UCHAR_MAX
            static int sum;
            sum = (int) (array[j].count); 
            sum += (int) (array[i].count);
            if(sum <= (int) UCHAR_MAX)
                array[j].count += array[i].count;
            else
                array[j].count = UCHAR_MAX;
        }
        else{
            j++;
            array[j].ID = array[i].ID;
            array[j].count = array[i].count;
        }
    }
    count = j+1;
}

template <typename StructDataType, typename KeyDataType,
          typename StructCompare, typename KeyCompare,
          typename SizeType>
void sort_kmers(StructDataType *&karray, SizeType &kcount, SizeType &ksize,
                MPI_Datatype mpi_struct_type,MPI_Datatype mpi_key_type,
                StructCompare structComparator,KeyCompare keyComparator,
                bool absentKmers, Para& params){

    int i = 0, j = 0;
    SizeType NoofElements;
    int kvalue = params.K;
    int size = MPI::COMM_WORLD.Get_size(),
        rank = MPI::COMM_WORLD.Get_rank();

    int threshold = params.tCard;

#ifdef DEBUG
    std::stringstream out1;
    out1 << std::setw(5) << rank << " "
         << std::setw(15) << kcount << " "
         << std::setw(15) << ksize << " "
         << std::setw(5) << (kcount > (long)size) << std::endl;
    //for(i=0;i<kcount; i++)
    //    out << karray[i].ID <<"\t"<< karray[i].count<<"\n";
    std::cout << out1.str();
    MPI_Barrier(MPI_COMM_WORLD);
#endif

    // load balancing code must appear here.
    load_balance2<StructDataType, SizeType>(karray,kcount,ksize,
                                            mpi_struct_type,
                                            size,rank);
    // sorting of the local k-mers after load balnce
    std::sort(karray, karray + kcount, structComparator);

    eliminate_dupes(karray,kcount);

    // Choosing size-1 splitters (size = no. of processors)
    // Assuming this processor has atleast size - 1 elements
#ifdef DEBUG
    std::stringstream out;
    out << std::setw(5) << rank << " "
        << std::setw(15) << kcount << " "
        << std::setw(15) << ksize << " " 
        << std::setw(5) << (kcount > (long)size) << std::endl;
    //for(i=0;i<kcount; i++)
    //    out << karray[i].ID <<"\t"<< karray[i].count<<"\n";
    std::cout << out.str();
    MPI_Barrier(MPI_COMM_WORLD);
#endif

    assert(kcount > (long)size);
    KeyDataType *Splitter = new KeyDataType[size-1];
    for (i=0; i < (size-1); i++){
        Splitter[i] = karray[(kcount/size) * (i+1)].ID;
    }

    // Gather all the local splitters at root
    KeyDataType *AllSplitter = new KeyDataType[size*(size-1)];
    MPI_Gather (Splitter, size-1, mpi_key_type,
                AllSplitter, size-1, mpi_key_type, 0 , MPI_COMM_WORLD);

    KeyDataType *GlobalSplitter =  new KeyDataType[size-1];
    // Choose the global splitters
    if(rank == 0){
        std::sort(AllSplitter, AllSplitter + (size * (size-1)), keyComparator);
        for (i=0; i<size-1; i++)
            GlobalSplitter[i] = AllSplitter[(size-1)*(i+1)];
#ifdef DEBUG
        std::cout << "Global Splitters : ";
        for(i=0;i<size -1; i++)
            std::cout << GlobalSplitter[i] << " ";
        std::cout << std::endl;
#endif
    }

    // Broadcast Global Splitters
    MPI_Bcast (GlobalSplitter, size-1, mpi_key_type, 0, MPI_COMM_WORLD);

    // Calculate the number of elements that I, rank,
    // should send and recieve  from every one else
    long *lsendcts = new long[size]();


    int *recvcts =  new int[size]();
    int *sendcts = new int[size]();
    int *recvdisp = new int[size]();
    int *senddisp = new int[size]();
    j = 0;
    for(i = 0; i < kcount;i++){
        if(j < (size-1)){
            if (karray[i].ID < GlobalSplitter[j])
                lsendcts[j]++;
            else{
                j++;
                i--;
            }
        }
        else
            lsendcts[j]++;
    }
#ifdef DEBUG
    for(j = 0; j < size; j++){
        std::stringstream out;
        out << std::setw(5) << rank << " "
            << std::setw(5) << j << " "
            << std::setw(15) << lsendcts[j] << std::endl;
        std::cout << out.str();
    }
    MPI_Barrier(MPI_COMM_WORLD);
#endif

    long int_max = std::numeric_limits<int>::max();
    for(j = 0;j < size; j++){
        assert(lsendcts[j] < int_max);
        sendcts[j] = (int)lsendcts[j];
    }
    // Do an all-to-all to get how much I,rank, will recieve from every one
    MPI_Alltoall(sendcts, 1 , MPI_INT,recvcts, 1 , MPI_INT, MPI_COMM_WORLD);

#ifdef DEBUG
    for(i = 0; i < size;i++) {
        std::stringstream out;
        out << "Proc " << rank << " : sendcts : "  <<  sendcts[i]
            << " recvcts  : " << recvcts[i] << std::endl;
        std::cout << out.str();
    }
#endif
    // Set-up offsets and do an all-to-all-v
    recvdisp[0]=0;
    senddisp[0]=0;
    for(i=1;i<size;i++){
        senddisp[i]=senddisp[i-1]+sendcts[i-1];
        recvdisp[i]=recvdisp[i-1]+recvcts[i-1];
    }

    // a new local
    SizeType NewlocalCount = 0;
    for(i=0;i<size;i++)
        NewlocalCount += recvcts[i];
    assert(NewlocalCount <= int_max);

#ifdef DEBUG
    std::stringstream outx;
    outx << std::setw(5) << rank << " "
         << std::setw(15) << NewlocalCount << " "
         << std::endl;
    //for(i=0;i<kcount; i++)
    //    out << karray[i].ID <<"\t"<< karray[i].count<<"\n";
    std::cout << outx.str();
    MPI_Barrier(MPI_COMM_WORLD);
#endif

    StructDataType *Newlocal = 
        (StructDataType*) malloc (NewlocalCount * sizeof(StructDataType));
    MPI_Alltoallv(karray,sendcts,senddisp,mpi_struct_type,
                  Newlocal,recvcts,recvdisp,mpi_struct_type,MPI_COMM_WORLD);
    std::sort(Newlocal,Newlocal+NewlocalCount,structComparator);

    // Get rid of the memory
    free(karray); kcount = ksize = 0;
    ksize = NewlocalCount;

    // eliminate dupes and update count
    eliminate_dupes(Newlocal,NewlocalCount);

#ifdef DEBUG
    MPI_Allgather (&NewlocalCount, 1 , MPI_INT,recvcts, 1 , MPI_INT, MPI_COMM_WORLD);
    NoofElements = recvcts[0];
    for(i=1;i<size;i++){
        NoofElements += recvcts[i];
    }
    if (rank == 0) {
        std::stringstream out12;
        out12 << "PROC : " << rank << " TOTAL BEFORE THRESHOLD "
              << NoofElements << std::endl;
        std::cout << out12.str();
    }
#endif
    eliminate_threshold(Newlocal,NewlocalCount,threshold);

    NoofElements = 0;
    int nlocal = NewlocalCount;
    // Gather for absent kmers
    MPI_Allgather (&nlocal, 1 , MPI_INT,recvcts, 1 , MPI_INT, MPI_COMM_WORLD);
    NoofElements = recvcts[0];
    for(i=1;i<size;i++){
        NoofElements += recvcts[i];
    }
#ifndef DEBUG
    if (rank == 0) {
        std::stringstream out12;
        out12 << "raw count\t" << NoofElements << std::endl;
        std::cout << out12.str();
    }
#endif
    StructDataType *AbsentIDs = 0;
    SizeType s_unit = 1;
    if( absentKmers &&
        NoofElements > (s_unit << (2 * kvalue - 1))) {
        KeyDataType kmax = 1;
        kmax = (sizeof(KeyDataType) * 8 == 2 * (unsigned) kvalue) ?
           std::numeric_limits<KeyDataType>::max() : (kmax << (2 * kvalue));
        KeyDataType si = (rank == 0) ? 0 : GlobalSplitter[rank-1]; j = 0;
        KeyDataType end = (rank == size-1) ? kmax : GlobalSplitter[rank];
        KeyDataType absent_size = end - si - NewlocalCount;
        if(absent_size > 0) {
            AbsentIDs = 
                (StructDataType*) malloc (absent_size * sizeof(StructDataType));
            SizeType k = 0;
            for(;si < end;si++){
                if(j < NewlocalCount && Newlocal[j].ID == si) {
                    j++;
                    continue;
                }
                AbsentIDs[k].ID = si; AbsentIDs[k].count = 0;
                k++;
            }
            free(Newlocal);
            Newlocal = AbsentIDs; NewlocalCount = k;
            ksize = absent_size;
        } else {
            free(Newlocal); Newlocal = 0;
            NewlocalCount = 0;
        }
        params.absentKmers = true;
    }
    karray = Newlocal;
    kcount = NewlocalCount;
    //
    // DONOT free Newlocal : Newlocal is the output!
    delete[] lsendcts;
    delete[] recvcts;
    delete[] sendcts;
    delete[] recvdisp;
    delete[] senddisp;
    delete[] AllSplitter;
    delete[] GlobalSplitter;
    delete[] Splitter;
}

template <typename StructDataType, typename KeyDataType, typename SizeType>
void gather_spectrum(StructDataType *&karray, SizeType &kcount, SizeType &ksize,
                     MPI_Datatype mpi_struct_type)
{
    StructDataType *AllData = 0;
    SizeType NoofElements = 0;
    StructDataType *Newlocal = karray;
    int NewlocalCount = kcount;
    int size = MPI::COMM_WORLD.Get_size(),
        rank = MPI::COMM_WORLD.Get_rank();
    int *recvcts =  new int[size]();
    int *sendcts = new int[size]();
    int *recvdisp = new int[size]();
    int *senddisp = new int[size]();
    int *recvcurrentcts =  new int[size]();
    int *recvcurrentdisp = new int[size]();

    MPI_Allgather (&NewlocalCount, 1 , MPI_INT,recvcts, 1 , MPI_INT, MPI_COMM_WORLD);
    recvdisp[0] = 0;
    NoofElements = recvcts[0];
    int max  = 0 ;
    for(int i=1;i<size;i++){
        recvdisp[i]=recvdisp[i-1]+recvcts[i-1];
        NoofElements += recvcts[i];
        if(max < recvcts[i]) max =  recvcts[i];
    }

#ifdef DEBUG
    if (rank == 0) {
        std::stringstream out12;
        out12 << "PROC : " << rank << " TOTAL FINAL "
              << NoofElements << std::endl;
        std::cout << out12.str();
    }
#endif

    // Use malloc - since ECData uses malloc for now
    AllData = (StructDataType*) malloc(NoofElements * sizeof(StructDataType));

    //should be  replaced by Allgatherv if all processors need sorted data
    // Change all gather to gather

    // lets find the maximum
    int fixednumber = 16000000/size;
    int iterations  =  (max%fixednumber == 0 ?
            max/fixednumber : max/fixednumber +1);
    int displacement, currentlocalcount;

#ifdef DEBUG
    if(rank == 0){
        std::stringstream out15;
        out15 << "PROC : " << rank << " MAX " << max
              << " FIXED " << fixednumber << " ITERATIONS "
              << iterations << std::endl;
        std::cout << out15.str();
    }
#endif

    if(size > 1) {
        for(int i = 0; i <iterations;i++) {
            if(i*fixednumber < NewlocalCount)
                displacement = i*fixednumber;
            else
                displacement = 0;

            for(int j = 0 ; j< size; j++){
                recvcurrentcts[j] = recvcts[j] - i*fixednumber > fixednumber ?
                     fixednumber : (recvcts[j] - i*fixednumber > 0 ?
                                        recvcts[j] - i*fixednumber: 0) ;
                recvcurrentdisp[j] = recvdisp[j]+ i*fixednumber;
            }
            currentlocalcount = recvcurrentcts[rank];

            MPI_Allgatherv(Newlocal + displacement, currentlocalcount, mpi_struct_type,
                        AllData, recvcurrentcts, recvcurrentdisp, mpi_struct_type,
                         MPI_COMM_WORLD);
        }
    } else {
       memcpy(AllData, Newlocal,NoofElements*sizeof(StructDataType));
    }

    // load the sorted list back onto input list
    free(karray); kcount = ksize = 0; Newlocal = 0;
    karray = AllData;
    ksize = kcount = NoofElements;

#ifdef DEBUG
    MPI_Barrier(MPI_COMM_WORLD);

    if(rank == 0){
        std::cout << "SORTED LIST OF KMER AND FREQUENCY: " <<
            NoofElements << std::endl;
        for(i=0;i < NoofElements; i++)
            std::cout << AllData[i].ID << " " << ((int)AllData[i].count) << std::endl;
    }
#endif
    //if(rank == 0)
    //    std::cout << "SORTED LIST OF KMER AND FREQUENCY: " <<
    //        NoofElements << std::endl;

    // DONOT free AllData : AllData is the output!
    delete[] recvcts;
    delete[] sendcts;
    delete[] recvdisp;
    delete[] senddisp;
    delete[] recvcurrentdisp;
    delete[] recvcurrentcts;
}

class ECData;
void sort_kmers(ECData& ecdata);
void dist_kmer_spectrum(ECData& ecdata);
void dist_tile_spectrum(ECData& ecdata);
#endif

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Written (W) 1999-2008 Soeren Sonnenburg
 * Written (W) 1999-2008 Gunnar Raetsch
 * Copyright (C) 1999-2008 Fraunhofer Institute FIRST and Max-Planck-Society
 */

#include "lib/common.h"
#include "lib/io.h"
#include "lib/Signal.h"
#include "lib/Trie.h"
#include "base/Parallel.h"

#include "kernel/WeightedDegreePositionStringKernel.h"
#include "kernel/SqrtDiagKernelNormalizer.h"
#include "features/Features.h"
#include "features/StringFeatures.h"

#include "classifier/svm/SVM.h"

#ifndef WIN32
#include <pthread.h>
#endif

#define TRIES(X) ((use_poim_tries) ? (poim_tries.X) : (tries.X))

template <class Trie> struct S_THREAD_PARAM 
{
	int32_t* vec;
	float64_t* result;
	float64_t* weights;
	CWeightedDegreePositionStringKernel* kernel;
	CTrie<Trie>* tries;
	float64_t factor;
	int32_t j;
	int32_t start;
	int32_t end;
	int32_t length;
	int32_t max_shift;
	int32_t* shift;
	int32_t* vec_idx;
};

CWeightedDegreePositionStringKernel::CWeightedDegreePositionStringKernel(
	int32_t size, int32_t d, int32_t mm, int32_t mkls)
: CStringKernel<char>(size), weights(NULL), position_weights(NULL),
	position_weights_lhs(NULL), position_weights_rhs(NULL),
	weights_buffer(NULL), mkl_stepsize(mkls), degree(d), length(0),
	max_mismatch(mm), seq_length(0), shift(NULL), shift_len(0),
	num_block_weights_external(0), block_weights_external(NULL),
	block_weights(NULL), type(E_EXTERNAL), tries(d), poim_tries(d),
	tree_initialized(false), use_poim_tries(false), m_poim_distrib(NULL),
	m_poim(NULL), m_poim_num_sym(0), m_poim_num_feat(0), m_poim_result_len(0),
	alphabet(NULL)
{
	properties |= KP_LINADD | KP_KERNCOMBINATION | KP_BATCHEVALUATION;
	set_wd_weights();
	ASSERT(weights);

	set_normalizer(new CSqrtDiagKernelNormalizer());
}

CWeightedDegreePositionStringKernel::CWeightedDegreePositionStringKernel(
	int32_t size, float64_t* w, int32_t d, int32_t mm, int32_t* s, int32_t sl,
	int32_t mkls)
: CStringKernel<char>(size), weights(NULL), position_weights(NULL),
	position_weights_lhs(NULL), position_weights_rhs(NULL),
	weights_buffer(NULL), mkl_stepsize(mkls), degree(d), length(0),
	max_mismatch(mm), seq_length(0), shift(NULL), shift_len(0),
	num_block_weights_external(0), block_weights_external(NULL),
	block_weights(NULL), type(E_EXTERNAL), tries(d), poim_tries(d),
	tree_initialized(false), use_poim_tries(false), m_poim_distrib(NULL),
	m_poim(NULL), m_poim_num_sym(0), m_poim_num_feat(0), m_poim_result_len(0),
	alphabet(NULL)
{
	properties |= KP_LINADD | KP_KERNCOMBINATION | KP_BATCHEVALUATION;

	weights=new float64_t[d*(1+max_mismatch)];
	for (int32_t i=0; i<d*(1+max_mismatch); i++)
		weights[i]=w[i];

	set_shifts(s, sl);

	set_normalizer(new CSqrtDiagKernelNormalizer());
}

CWeightedDegreePositionStringKernel::CWeightedDegreePositionStringKernel(
	CStringFeatures<char>* l, CStringFeatures<char>* r, int32_t d)
: CStringKernel<char>(10), weights(NULL), position_weights(NULL),
	position_weights_lhs(NULL), position_weights_rhs(NULL),
	weights_buffer(NULL), mkl_stepsize(1), degree(d), length(0),
	max_mismatch(0), seq_length(0), shift(NULL), shift_len(0),
	num_block_weights_external(0), block_weights_external(NULL),
	block_weights(NULL), type(E_EXTERNAL), tries(d), poim_tries(d),
	tree_initialized(false), use_poim_tries(false), m_poim_distrib(NULL),
	m_poim(NULL), m_poim_num_sym(0), m_poim_num_feat(0), m_poim_result_len(0),
	alphabet(NULL)
{
	properties |= KP_LINADD | KP_KERNCOMBINATION | KP_BATCHEVALUATION;
	set_wd_weights();
	ASSERT(weights);
	set_normalizer(new CSqrtDiagKernelNormalizer());

	init(l, r);
}


CWeightedDegreePositionStringKernel::~CWeightedDegreePositionStringKernel()
{
	cleanup();
	cleanup_POIM2();

	delete[] shift;
	shift=NULL;

	delete[] weights;
	weights=NULL ;

	delete[] block_weights;
	block_weights=NULL;

	delete[] position_weights;
	position_weights=NULL;

	delete[] position_weights_lhs;
	position_weights_lhs=NULL;

	delete[] position_weights_rhs;
	position_weights_rhs=NULL;

	delete[] weights_buffer;
	weights_buffer=NULL;
}

void CWeightedDegreePositionStringKernel::remove_lhs()
{
	SG_DEBUG( "deleting CWeightedDegreePositionStringKernel optimization\n");
	delete_optimization();

	tries.destroy();
	poim_tries.destroy();

	CKernel::remove_lhs();
}

void CWeightedDegreePositionStringKernel::create_empty_tries()
{
	ASSERT(lhs);
	seq_length = ((CStringFeatures<char>*) lhs)->get_max_vector_length();

	if (opt_type==SLOWBUTMEMEFFICIENT)
	{
		tries.create(seq_length, true); 
		poim_tries.create(seq_length, true); 
	}
	else if (opt_type==FASTBUTMEMHUNGRY)
	{
		tries.create(seq_length, false);  // still buggy
		poim_tries.create(seq_length, false);  // still buggy
	}
	else
		SG_ERROR( "unknown optimization type\n");
}

bool CWeightedDegreePositionStringKernel::init(CFeatures* l, CFeatures* r)
{
	int32_t lhs_changed = (lhs!=l) ;
	int32_t rhs_changed = (rhs!=r) ;

	CStringKernel<char>::init(l,r);

	SG_DEBUG( "lhs_changed: %i\n", lhs_changed) ;
	SG_DEBUG( "rhs_changed: %i\n", rhs_changed) ;

	CStringFeatures<char>* sf_l=(CStringFeatures<char>*) l;
	CStringFeatures<char>* sf_r=(CStringFeatures<char>*) r;

	/* set shift */
	if (shift_len==0) {
		shift_len=sf_l->get_vector_length(0);
		int32_t *shifts=new int32_t[shift_len];
		for (int32_t i=0; i<shift_len; i++) {
			shifts[i]=1;
		}
		set_shifts(shifts, shift_len);
		delete[] shifts;
	}


	int32_t len=sf_l->get_max_vector_length();
	if (lhs_changed && !sf_l->have_same_length(len))
		SG_ERROR("All strings in WD kernel must have same length (lhs wrong)!\n");

	if (rhs_changed && !sf_r->have_same_length(len))
		SG_ERROR("All strings in WD kernel must have same length (rhs wrong)!\n");

	delete alphabet;
	alphabet= new CAlphabet(sf_l->get_alphabet());
	CAlphabet* ralphabet=sf_r->get_alphabet();
	if (!((alphabet->get_alphabet()==DNA) || (alphabet->get_alphabet()==RNA)))
		properties &= ((uint64_t) (-1)) ^ (KP_LINADD | KP_BATCHEVALUATION);

	ASSERT(ralphabet->get_alphabet()==alphabet->get_alphabet());
	SG_UNREF(ralphabet);

	//whenever init is called also init tries and block weights
	create_empty_tries();
	init_block_weights();

	return init_normalizer();
}

void CWeightedDegreePositionStringKernel::cleanup()
{
	SG_DEBUG( "deleting CWeightedDegreePositionStringKernel optimization\n");
	delete_optimization();

	delete[] block_weights;
	block_weights=NULL;

	tries.destroy();
	poim_tries.destroy();

	seq_length = 0;
	tree_initialized = false;

	delete alphabet;
	alphabet=NULL;

	CKernel::cleanup();
}

bool CWeightedDegreePositionStringKernel::load_init(FILE* src)
{
	return false;
}

bool CWeightedDegreePositionStringKernel::save_init(FILE* dest)
{
	return false;
}

bool CWeightedDegreePositionStringKernel::init_optimization(
	int32_t p_count, int32_t * IDX, float64_t * alphas, int32_t tree_num,
	int32_t upto_tree)
{
	ASSERT(position_weights_lhs==NULL);
	ASSERT(position_weights_rhs==NULL);

	if (upto_tree<0)
		upto_tree=tree_num;

	if (max_mismatch!=0)
	{
		SG_ERROR( "CWeightedDegreePositionStringKernel optimization not implemented for mismatch!=0\n");
		return false ;
	}

	if (tree_num<0)
		SG_DEBUG( "deleting CWeightedDegreePositionStringKernel optimization\n");

	delete_optimization();

	if (tree_num<0)
		SG_DEBUG( "initializing CWeightedDegreePositionStringKernel optimization\n") ;

	for (int32_t i=0; i<p_count; i++)
	{
		if (tree_num<0)
		{
			if ( (i % (p_count/10+1)) == 0)
				SG_PROGRESS(i,0,p_count);
			add_example_to_tree(IDX[i], alphas[i]);
		}
		else
		{
			for (int32_t t=tree_num; t<=upto_tree; t++)
				add_example_to_single_tree(IDX[i], alphas[i], t);
		}
	}

	if (tree_num<0)
		SG_DONE();

	set_is_initialized(true) ;
	return true ;
}

bool CWeightedDegreePositionStringKernel::delete_optimization() 
{ 
	if ((opt_type==FASTBUTMEMHUNGRY) && (tries.get_use_compact_terminal_nodes())) 
	{
		tries.set_use_compact_terminal_nodes(false) ;
		SG_DEBUG( "disabling compact trie nodes with FASTBUTMEMHUNGRY\n") ;
	}

	if (get_is_initialized())
	{
		if (opt_type==SLOWBUTMEMEFFICIENT)
			tries.delete_trees(true); 
		else if (opt_type==FASTBUTMEMHUNGRY)
			tries.delete_trees(false);  // still buggy
		else {
			SG_ERROR( "unknown optimization type\n");
		}
		set_is_initialized(false);

		return true;
	}

	return false;
}

float64_t CWeightedDegreePositionStringKernel::compute_with_mismatch(
	char* avec, int32_t alen, char* bvec, int32_t blen)
{
	float64_t max_shift_vec[max_shift];
    float64_t sum0=0 ;
    for (int32_t i=0; i<max_shift; i++)
		max_shift_vec[i]=0 ;
	
    // no shift
    for (int32_t i=0; i<alen; i++)
    {
		if ((position_weights!=NULL) && (position_weights[i]==0.0))
			continue ;
		
		int32_t mismatches=0;
		float64_t sumi = 0.0 ;
		for (int32_t j=0; (j<degree) && (i+j<alen); j++)
		{
			if (avec[i+j]!=bvec[i+j])
			{
				mismatches++ ;
				if (mismatches>max_mismatch)
					break ;
			} ;
			sumi += weights[j+degree*mismatches];
		}
		if (position_weights!=NULL)
			sum0 += position_weights[i]*sumi ;
		else
			sum0 += sumi ;
    } ;
	
    for (int32_t i=0; i<alen; i++)
    {
		for (int32_t k=1; (k<=shift[i]) && (i+k<alen); k++)
		{
			if ((position_weights!=NULL) && (position_weights[i]==0.0) && (position_weights[i+k]==0.0))
				continue ;
			
			float64_t sumi1 = 0.0 ;
			// shift in sequence a
			int32_t mismatches=0;
			for (int32_t j=0; (j<degree) && (i+j+k<alen); j++)
			{
				if (avec[i+j+k]!=bvec[i+j])
				{
					mismatches++ ;
					if (mismatches>max_mismatch)
						break ;
				} ;
				sumi1 += weights[j+degree*mismatches];
			}
			float64_t sumi2 = 0.0 ;
			// shift in sequence b
			mismatches=0;
			for (int32_t j=0; (j<degree) && (i+j+k<alen); j++)
			{
				if (avec[i+j]!=bvec[i+j+k])
				{
					mismatches++ ;
					if (mismatches>max_mismatch)
						break ;
				} ;
				sumi2 += weights[j+degree*mismatches];
			}
			if (position_weights!=NULL)
				max_shift_vec[k-1] += position_weights[i]*sumi1 + position_weights[i+k]*sumi2 ;
			else
				max_shift_vec[k-1] += sumi1 + sumi2 ;
		} ;
    }
	
    float64_t result = sum0 ;
    for (int32_t i=0; i<max_shift; i++)
		result += max_shift_vec[i]/(2*(i+1)) ;
	
    return result ;
}

float64_t CWeightedDegreePositionStringKernel::compute_without_mismatch(
	char* avec, int32_t alen, char* bvec, int32_t blen) 
{
	float64_t max_shift_vec[max_shift];
	float64_t sum0=0 ;
	for (int32_t i=0; i<max_shift; i++)
		max_shift_vec[i]=0 ;

	// no shift
	for (int32_t i=0; i<alen; i++)
	{
		if ((position_weights!=NULL) && (position_weights[i]==0.0))
			continue ;

		float64_t sumi = 0.0 ;
		for (int32_t j=0; (j<degree) && (i+j<alen); j++)
		{
			if (avec[i+j]!=bvec[i+j])
				break ;
			sumi += weights[j];
		}
		if (position_weights!=NULL)
			sum0 += position_weights[i]*sumi ;
		else
			sum0 += sumi ;
	} ;

	for (int32_t i=0; i<alen; i++)
	{
		for (int32_t k=1; (k<=shift[i]) && (i+k<alen); k++)
		{
			if ((position_weights!=NULL) && (position_weights[i]==0.0) && (position_weights[i+k]==0.0))
				continue ;

			float64_t sumi1 = 0.0 ;
			// shift in sequence a
			for (int32_t j=0; (j<degree) && (i+j+k<alen); j++)
			{
				if (avec[i+j+k]!=bvec[i+j])
					break ;
				sumi1 += weights[j];
			}
			float64_t sumi2 = 0.0 ;
			// shift in sequence b
			for (int32_t j=0; (j<degree) && (i+j+k<alen); j++)
			{
				if (avec[i+j]!=bvec[i+j+k])
					break ;
				sumi2 += weights[j];
			}
			if (position_weights!=NULL)
				max_shift_vec[k-1] += position_weights[i]*sumi1 + position_weights[i+k]*sumi2 ;
			else
				max_shift_vec[k-1] += sumi1 + sumi2 ;
		} ;
	}

	float64_t result = sum0 ;
	for (int32_t i=0; i<max_shift; i++)
		result += max_shift_vec[i]/(2*(i+1)) ;

	return result ;
}

float64_t CWeightedDegreePositionStringKernel::compute_without_mismatch_matrix(
	char* avec, int32_t alen, char* bvec, int32_t blen) 
{
	float64_t max_shift_vec[max_shift];
	float64_t sum0=0 ;
	for (int32_t i=0; i<max_shift; i++)
		max_shift_vec[i]=0 ;

	// no shift
	for (int32_t i=0; i<alen; i++)
	{
		if ((position_weights!=NULL) && (position_weights[i]==0.0))
			continue ;
		float64_t sumi = 0.0 ;
		for (int32_t j=0; (j<degree) && (i+j<alen); j++)
		{
			if (avec[i+j]!=bvec[i+j])
				break ;
			sumi += weights[i*degree+j];
		}
		if (position_weights!=NULL)
			sum0 += position_weights[i]*sumi ;
		else
			sum0 += sumi ;
	} ;

	for (int32_t i=0; i<alen; i++)
	{
		for (int32_t k=1; (k<=shift[i]) && (i+k<alen); k++)
		{
			if ((position_weights!=NULL) && (position_weights[i]==0.0) && (position_weights[i+k]==0.0))
				continue ;

			float64_t sumi1 = 0.0 ;
			// shift in sequence a
			for (int32_t j=0; (j<degree) && (i+j+k<alen); j++)
			{
				if (avec[i+j+k]!=bvec[i+j])
					break ;
				sumi1 += weights[i*degree+j];
			}
			float64_t sumi2 = 0.0 ;
			// shift in sequence b
			for (int32_t j=0; (j<degree) && (i+j+k<alen); j++)
			{
				if (avec[i+j]!=bvec[i+j+k])
					break ;
				sumi2 += weights[i*degree+j];
			}
			if (position_weights!=NULL)
				max_shift_vec[k-1] += position_weights[i]*sumi1 + position_weights[i+k]*sumi2 ;
			else
				max_shift_vec[k-1] += sumi1 + sumi2 ;
		} ;
	}

	float64_t result = sum0 ;
	for (int32_t i=0; i<max_shift; i++)
		result += max_shift_vec[i]/(2*(i+1)) ;

	return result ;
}

float64_t CWeightedDegreePositionStringKernel::compute_without_mismatch_position_weights(
	char* avec, float64_t* pos_weights_lhs, int32_t alen, char* bvec,
	float64_t* pos_weights_rhs, int32_t blen)
{
	float64_t max_shift_vec[max_shift];
	float64_t sum0=0 ;
	for (int32_t i=0; i<max_shift; i++)
		max_shift_vec[i]=0 ;

	// no shift
	for (int32_t i=0; i<alen; i++)
	{
		if ((position_weights!=NULL) && (position_weights[i]==0.0))
			continue ;

		float64_t sumi = 0.0 ;
	    float64_t posweight_lhs = 0.0 ;
	    float64_t posweight_rhs = 0.0 ;
		for (int32_t j=0; (j<degree) && (i+j<alen); j++)
		{
			posweight_lhs += pos_weights_lhs[i+j] ;
			posweight_rhs += pos_weights_rhs[i+j] ;
			
			if (avec[i+j]!=bvec[i+j])
				break ;
			sumi += weights[j]*(posweight_lhs/(j+1))*(posweight_rhs/(j+1)) ;
		}
		if (position_weights!=NULL)
			sum0 += position_weights[i]*sumi ;
		else
			sum0 += sumi ;
	} ;

	for (int32_t i=0; i<alen; i++)
	{
		for (int32_t k=1; (k<=shift[i]) && (i+k<alen); k++)
		{
			if ((position_weights!=NULL) && (position_weights[i]==0.0) && (position_weights[i+k]==0.0))
				continue ;

			// shift in sequence a	
			float64_t sumi1 = 0.0 ;
			float64_t posweight_lhs = 0.0 ;
			float64_t posweight_rhs = 0.0 ;
			for (int32_t j=0; (j<degree) && (i+j+k<alen); j++)
			{
				posweight_lhs += pos_weights_lhs[i+j+k] ;
				posweight_rhs += pos_weights_rhs[i+j] ;
				if (avec[i+j+k]!=bvec[i+j])
					break ;
				sumi1 += weights[j]*(posweight_lhs/(j+1))*(posweight_rhs/(j+1)) ;
			}
			// shift in sequence b
			float64_t sumi2 = 0.0 ;
			posweight_lhs = 0.0 ;
			posweight_rhs = 0.0 ;
			for (int32_t j=0; (j<degree) && (i+j+k<alen); j++)
			{
				posweight_lhs += pos_weights_lhs[i+j] ;
				posweight_rhs += pos_weights_rhs[i+j+k] ;
				if (avec[i+j]!=bvec[i+j+k])
					break ;
				sumi2 += weights[j]*(posweight_lhs/(j+1))*(posweight_rhs/(j+1)) ;
			}
			if (position_weights!=NULL)
				max_shift_vec[k-1] += position_weights[i]*sumi1 + position_weights[i+k]*sumi2 ;
			else
				max_shift_vec[k-1] += sumi1 + sumi2 ;
		} ;
	}

	float64_t result = sum0 ;
	for (int32_t i=0; i<max_shift; i++)
		result += max_shift_vec[i]/(2*(i+1)) ;

	return result ;
}


float64_t CWeightedDegreePositionStringKernel::compute(
	int32_t idx_a, int32_t idx_b)
{
	int32_t alen, blen;

	char* avec=((CStringFeatures<char>*) lhs)->get_feature_vector(idx_a, alen);
	char* bvec=((CStringFeatures<char>*) rhs)->get_feature_vector(idx_b, blen);
	// can only deal with strings of same length
	ASSERT(alen==blen);
	ASSERT(shift_len==alen);

	float64_t result = 0 ;
	if (position_weights_lhs!=NULL || position_weights_rhs!=NULL)
	{
		ASSERT(max_mismatch==0);
		result = compute_without_mismatch_position_weights(avec, &position_weights_lhs[idx_a*alen], alen, bvec, &position_weights_rhs[idx_b*blen], blen) ;
	}
	else if (max_mismatch > 0)
		result = compute_with_mismatch(avec, alen, bvec, blen) ;
	else if (length==0)
		result = compute_without_mismatch(avec, alen, bvec, blen) ;
	else
		result = compute_without_mismatch_matrix(avec, alen, bvec, blen) ;

	return result ;
}


void CWeightedDegreePositionStringKernel::add_example_to_tree(
	int32_t idx, float64_t alpha)
{
	ASSERT(position_weights_lhs==NULL);
	ASSERT(position_weights_rhs==NULL);
	ASSERT(alphabet);
	ASSERT(alphabet->get_alphabet()==DNA || alphabet->get_alphabet()==RNA);

	int32_t len=0;
	char* char_vec=((CStringFeatures<char>*) lhs)->get_feature_vector(idx, len);
	ASSERT(max_mismatch==0);
	int32_t *vec = new int32_t[len] ;

	for (int32_t i=0; i<len; i++)
		vec[i]=alphabet->remap_to_bin(char_vec[i]);

	if (opt_type==FASTBUTMEMHUNGRY)
	{
		//TRIES(set_use_compact_terminal_nodes(false)) ;
		ASSERT(!TRIES(get_use_compact_terminal_nodes()));
	}

	for (int32_t i=0; i<len; i++)
	{
		int32_t max_s=-1;

		if (opt_type==SLOWBUTMEMEFFICIENT)
			max_s=0;
		else if (opt_type==FASTBUTMEMHUNGRY)
			max_s=shift[i];
		else {
			SG_ERROR( "unknown optimization type\n");
		}

		for (int32_t s=max_s; s>=0; s--)
		{
			float64_t alpha_pw = normalizer->normalize_lhs((s==0) ? (alpha) : (alpha/(2.0*s)), idx);
			TRIES(add_to_trie(i, s, vec, alpha_pw, weights, (length!=0))) ;
			//fprintf(stderr, "tree=%i, s=%i, example=%i, alpha_pw=%1.2f\n", i, s, idx, alpha_pw) ;

			if ((s==0) || (i+s>=len))
				continue;

			TRIES(add_to_trie(i+s, -s, vec, alpha_pw, weights, (length!=0))) ;
			//fprintf(stderr, "tree=%i, s=%i, example=%i, alpha_pw=%1.2f\n", i+s, -s, idx, alpha_pw) ;
		}
	}

	delete[] vec ;
	tree_initialized=true ;
}

void CWeightedDegreePositionStringKernel::add_example_to_single_tree(
	int32_t idx, float64_t alpha, int32_t tree_num) 
{
	ASSERT(position_weights_lhs==NULL);
	ASSERT(position_weights_rhs==NULL);
	ASSERT(alphabet);
	ASSERT(alphabet->get_alphabet()==DNA || alphabet->get_alphabet()==RNA);

	int32_t len=0;
	char* char_vec=((CStringFeatures<char>*) lhs)->get_feature_vector(idx, len);
	ASSERT(max_mismatch==0);
	int32_t *vec=new int32_t[len];
	int32_t max_s=-1;

	if (opt_type==SLOWBUTMEMEFFICIENT)
		max_s=0;
	else if (opt_type==FASTBUTMEMHUNGRY)
	{
		ASSERT(!tries.get_use_compact_terminal_nodes());
		max_s=shift[tree_num];
	}
	else {
		SG_ERROR( "unknown optimization type\n");
	}
	for (int32_t i=CMath::max(0,tree_num-max_shift); i<CMath::min(len,tree_num+degree+max_shift); i++)
		vec[i]=alphabet->remap_to_bin(char_vec[i]);

	for (int32_t s=max_s; s>=0; s--)
	{
		float64_t alpha_pw = normalizer->normalize_lhs((s==0) ? (alpha) : (alpha/(2.0*s)), idx);
		tries.add_to_trie(tree_num, s, vec, alpha_pw, weights, (length!=0)) ;
		//fprintf(stderr, "tree=%i, s=%i, example=%i, alpha_pw=%1.2f\n", tree_num, s, idx, alpha_pw) ;
	} 

	if (opt_type==FASTBUTMEMHUNGRY)
	{
		for (int32_t i=CMath::max(0,tree_num-max_shift); i<CMath::min(len,tree_num+max_shift+1); i++)
		{
			int32_t s=tree_num-i;
			if ((i+s<len) && (s>=1) && (s<=shift[i]))
			{
				float64_t alpha_pw = normalizer->normalize_lhs((s==0) ? (alpha) : (alpha/(2.0*s)), idx);
				tries.add_to_trie(tree_num, -s, vec, alpha_pw, weights, (length!=0)) ; 
				//fprintf(stderr, "tree=%i, s=%i, example=%i, alpha_pw=%1.2f\n", tree_num, -s, idx, alpha_pw) ;
			}
		}
	}
	delete[] vec ;
	tree_initialized=true ;
}

float64_t CWeightedDegreePositionStringKernel::compute_by_tree(int32_t idx)
{
	ASSERT(position_weights_lhs==NULL);
	ASSERT(position_weights_rhs==NULL);
	ASSERT(alphabet);
	ASSERT(alphabet->get_alphabet()==DNA || alphabet->get_alphabet()==RNA);

	float64_t sum=0;
	int32_t len=0;
	char* char_vec=((CStringFeatures<char>*) rhs)->get_feature_vector(idx, len);
	ASSERT(max_mismatch==0);
	int32_t *vec=new int32_t[len];

	for (int32_t i=0; i<len; i++)
		vec[i]=alphabet->remap_to_bin(char_vec[i]);

	for (int32_t i=0; i<len; i++)
		sum += tries.compute_by_tree_helper(vec, len, i, i, i, weights, (length!=0)) ;

	if (opt_type==SLOWBUTMEMEFFICIENT)
	{
		for (int32_t i=0; i<len; i++)
		{
			for (int32_t s=1; (s<=shift[i]) && (i+s<len); s++)
			{
				sum+=tries.compute_by_tree_helper(vec, len, i, i+s, i, weights, (length!=0))/(2*s) ;
				sum+=tries.compute_by_tree_helper(vec, len, i+s, i, i+s, weights, (length!=0))/(2*s) ;
			}
		}
	}

	delete[] vec ;

	return normalizer->normalize_rhs(sum, idx);
}

void CWeightedDegreePositionStringKernel::compute_by_tree(
	int32_t idx, float64_t* LevelContrib)
{
	ASSERT(position_weights_lhs==NULL);
	ASSERT(position_weights_rhs==NULL);
	ASSERT(alphabet);
	ASSERT(alphabet->get_alphabet()==DNA || alphabet->get_alphabet()==RNA);

	int32_t len=0;
	char* char_vec=((CStringFeatures<char>*) rhs)->get_feature_vector(idx, len);
	ASSERT(max_mismatch==0);
	int32_t *vec=new int32_t[len];

	for (int32_t i=0; i<len; i++)
		vec[i]=alphabet->remap_to_bin(char_vec[i]);

	for (int32_t i=0; i<len; i++)
	{
		tries.compute_by_tree_helper(vec, len, i, i, i, LevelContrib,
				normalizer->normalize_rhs(1.0, idx), mkl_stepsize, weights,
				(length!=0));
	}

	if (opt_type==SLOWBUTMEMEFFICIENT)
	{
		for (int32_t i=0; i<len; i++)
			for (int32_t k=1; (k<=shift[i]) && (i+k<len); k++)
			{
				tries.compute_by_tree_helper(vec, len, i, i+k, i, LevelContrib,
						normalizer->normalize_rhs(1.0/(2*k), idx), mkl_stepsize,
						weights, (length!=0)) ;
				tries.compute_by_tree_helper(vec, len, i+k, i, i+k,
						LevelContrib, normalizer->normalize_rhs(1.0/(2*k), idx),
						mkl_stepsize, weights, (length!=0)) ;
			}
	}

	delete[] vec ;
}

float64_t* CWeightedDegreePositionStringKernel::compute_abs_weights(
	int32_t &len)
{
	return tries.compute_abs_weights(len);
}

bool CWeightedDegreePositionStringKernel::set_shifts(
	int32_t* shift_, int32_t shift_len_)
{
	delete[] shift;

	shift_len = shift_len_ ;
	shift = new int32_t[shift_len] ;

	if (shift)
	{
		max_shift = 0 ;

		for (int32_t i=0; i<shift_len; i++)
		{
			shift[i] = shift_[i] ;
			if (shift[i]>max_shift)
				max_shift = shift[i] ;
		}

		ASSERT(max_shift>=0 && max_shift<=shift_len);
	}
	
	return false;
}

bool CWeightedDegreePositionStringKernel::set_wd_weights()
{
	ASSERT(degree>0);

	delete[] weights;
	weights=new float64_t[degree];
	if (weights)
	{
		int32_t i;
		float64_t sum=0;
		for (i=0; i<degree; i++)
		{
			weights[i]=degree-i;
			sum+=weights[i];
		}
		for (i=0; i<degree; i++)
			weights[i]/=sum;

		for (i=0; i<degree; i++)
		{
			for (int32_t j=1; j<=max_mismatch; j++)
			{
				if (j<i+1)
				{
					int32_t nk=CMath::nchoosek(i+1, j);
					weights[i+j*degree]=weights[i]/(nk*pow(3,j));
				}
				else
					weights[i+j*degree]= 0;
			}
		}

		return true;
	}
	else
		return false;
}

bool CWeightedDegreePositionStringKernel::set_weights(
	float64_t* ws, int32_t d, int32_t len)
{
	SG_DEBUG("degree = %i  d=%i\n", degree, d);
	degree=d;
	length=len;

	if (len <= 0)
		len=1;

	delete[] weights;
	weights=new float64_t[d*len];

	if (weights)
	{
		for (int32_t i=0; i<degree*len; i++)
			weights[i]=ws[i];
		return true;
	}
	else
		return false;
}

bool CWeightedDegreePositionStringKernel::set_position_weights(
	float64_t* pws, int32_t len)
{
	//fprintf(stderr, "len=%i\n", len);

	if (len==0)
	{
		delete[] position_weights;
		position_weights = NULL;
		tries.set_position_weights(position_weights);
		return true;
	}
	if (seq_length==0)
		seq_length=len;

	if (seq_length!=len)
	{
		SG_ERROR("seq_length = %i, position_weights_length=%i\n", seq_length, len);
		return false;
	}
	delete[] position_weights;
	position_weights=new float64_t[len];
	tries.set_position_weights(position_weights);

	if (position_weights)
	{
		for (int32_t i=0; i<len; i++)
			position_weights[i]=pws[i];
		return true;
	}
	else
		return false;
}

bool CWeightedDegreePositionStringKernel::set_position_weights_lhs(float64_t* pws, int32_t len, int32_t num)
{
	//fprintf(stderr, "lhs %i %i %i\n", len, num, seq_length);

	if (position_weights_rhs==position_weights_lhs)
		position_weights_rhs=NULL;
	else
		delete_position_weights_rhs();

	if (len==0)
	{
		return delete_position_weights_lhs();
	}
	
	if (seq_length!=len)
	{
		SG_ERROR("seq_length = %i, position_weights_length=%i\n", seq_length, len);
		return false;
	}
	if (!lhs)
	{
		SG_ERROR("lhs=NULL\n");
		return false ;
	}
	if (lhs->get_num_vectors()!=num)
	{
		SG_ERROR("lhs->get_num_vectors()=%i, num=%i\n", lhs->get_num_vectors(), num);
		return false;
	}
	
	delete[] position_weights_lhs;
	position_weights_lhs=new float64_t[len*num];
	if (position_weights_lhs)
	{
		for (int32_t i=0; i<len*num; i++)
			position_weights_lhs[i]=pws[i];
		return true;
	}
	else
		return false;
}

bool CWeightedDegreePositionStringKernel::set_position_weights_rhs(
	float64_t* pws, int32_t len, int32_t num)
{
	//fprintf(stderr, "rhs %i %i %i\n", len, num, seq_length);
	if (len==0)
	{
		if (position_weights_rhs==position_weights_lhs)
		{
			position_weights_rhs=NULL;
			return true;
		}
		return delete_position_weights_rhs();
	}

	if (seq_length!=len)
	{
		SG_ERROR("seq_length = %i, position_weights_length=%i\n", seq_length, len);
		return false;
	}
	if (!rhs)
	{
		if (!lhs)
		{
			SG_ERROR("rhs==0 and lhs=NULL\n");
			return false;
		}
		if (lhs->get_num_vectors()!=num)
		{
			SG_ERROR("lhs->get_num_vectors()=%i, num=%i\n", lhs->get_num_vectors(), num);
			return false;
		}
	} else
		if (rhs->get_num_vectors()!=num)
		{
			SG_ERROR("rhs->get_num_vectors()=%i, num=%i\n", rhs->get_num_vectors(), num);
			return false;
		}

	delete[] position_weights_rhs;
	position_weights_rhs=new float64_t[len*num];
	if (position_weights_rhs)
	{
		for (int32_t i=0; i<len*num; i++)
			position_weights_rhs[i]=pws[i];
		return true;
	}
	else
		return false;
}

bool CWeightedDegreePositionStringKernel::init_block_weights_from_wd()
{
	delete[] block_weights;
	block_weights=new float64_t[CMath::max(seq_length,degree)];

	if (block_weights)
	{
		int32_t k;
		float64_t d=degree; // use float to evade rounding errors below

		for (k=0; k<degree; k++)
			block_weights[k]=
				(-pow(k, 3)+(3*d-3)*pow(k, 2)+(9*d-2)*k+6*d)/(3*d*(d+1));
		for (k=degree; k<seq_length; k++)
			block_weights[k]=(-d+3*k+4)/3;
	}

	return (block_weights!=NULL);
}

bool CWeightedDegreePositionStringKernel::init_block_weights_from_wd_external()
{
	ASSERT(weights);
	delete[] block_weights;
	block_weights=new float64_t[CMath::max(seq_length,degree)];

	if (block_weights)
	{
		int32_t i=0;
		block_weights[0]=weights[0];
		for (i=1; i<CMath::max(seq_length,degree); i++)
			block_weights[i]=0;

		for (i=1; i<CMath::max(seq_length,degree); i++)
		{
			block_weights[i]=block_weights[i-1];

			float64_t contrib=0;
			for (int32_t j=0; j<CMath::min(degree,i+1); j++)
				contrib+=weights[j];

			block_weights[i]+=contrib;
		}
	}

	return (block_weights!=NULL);
}

bool CWeightedDegreePositionStringKernel::init_block_weights_const()
{
	delete[] block_weights;
	block_weights=new float64_t[seq_length];

	if (block_weights)
	{
		for (int32_t i=1; i<seq_length+1 ; i++)
			block_weights[i-1]=1.0/seq_length;
	}

	return (block_weights!=NULL);
}

bool CWeightedDegreePositionStringKernel::init_block_weights_linear()
{
	delete[] block_weights;
	block_weights=new float64_t[seq_length];

	if (block_weights)
	{
		for (int32_t i=1; i<seq_length+1 ; i++)
			block_weights[i-1]=degree*i;
	}

	return (block_weights!=NULL);
}

bool CWeightedDegreePositionStringKernel::init_block_weights_sqpoly()
{
	delete[] block_weights;
	block_weights=new float64_t[seq_length];

	if (block_weights)
	{
		for (int32_t i=1; i<degree+1 ; i++)
			block_weights[i-1]=((float64_t) i)*i;

		for (int32_t i=degree+1; i<seq_length+1 ; i++)
			block_weights[i-1]=i;
	}

	return (block_weights!=NULL);
}

bool CWeightedDegreePositionStringKernel::init_block_weights_cubicpoly()
{
	delete[] block_weights;
	block_weights=new float64_t[seq_length];

	if (block_weights)
	{
		for (int32_t i=1; i<degree+1 ; i++)
			block_weights[i-1]=((float64_t) i)*i*i;

		for (int32_t i=degree+1; i<seq_length+1 ; i++)
			block_weights[i-1]=i;
	}

	return (block_weights!=NULL);
}

bool CWeightedDegreePositionStringKernel::init_block_weights_exp()
{
	delete[] block_weights;
	block_weights=new float64_t[seq_length];

	if (block_weights)
	{
		for (int32_t i=1; i<degree+1 ; i++)
			block_weights[i-1]=exp(((float64_t) i/10.0));

		for (int32_t i=degree+1; i<seq_length+1 ; i++)
			block_weights[i-1]=i;
	}

	return (block_weights!=NULL);
}

bool CWeightedDegreePositionStringKernel::init_block_weights_log()
{
	delete[] block_weights;
	block_weights=new float64_t[seq_length];

	if (block_weights)
	{
		for (int32_t i=1; i<degree+1 ; i++)
			block_weights[i-1]=pow(log(i),2);

		for (int32_t i=degree+1; i<seq_length+1 ; i++)
			block_weights[i-1]=i-degree+1+pow(log(degree+1),2);
	}

	return (block_weights!=NULL);
}

bool CWeightedDegreePositionStringKernel::init_block_weights_external()
{
	if (block_weights_external && (seq_length == num_block_weights_external) )
	{
		delete[] block_weights;
		block_weights=new float64_t[seq_length];

		if (block_weights)
		{
			for (int32_t i=0; i<seq_length; i++)
				block_weights[i]=block_weights_external[i];
		}
	}
	else {
      SG_ERROR( "sequence longer then weights (seqlen:%d, wlen:%d)\n", seq_length, block_weights_external);
   }
	return (block_weights!=NULL);
}

bool CWeightedDegreePositionStringKernel::init_block_weights()
{
	switch (type)
	{
		case E_WD:
			return init_block_weights_from_wd();
		case E_EXTERNAL:
			return init_block_weights_from_wd_external();
		case E_BLOCK_CONST:
			return init_block_weights_const();
		case E_BLOCK_LINEAR:
			return init_block_weights_linear();
		case E_BLOCK_SQPOLY:
			return init_block_weights_sqpoly();
		case E_BLOCK_CUBICPOLY:
			return init_block_weights_cubicpoly();
		case E_BLOCK_EXP:
			return init_block_weights_exp();
		case E_BLOCK_LOG:
			return init_block_weights_log();
		case E_BLOCK_EXTERNAL:
			return init_block_weights_external();
		default:
			return false;
	};
}



void* CWeightedDegreePositionStringKernel::compute_batch_helper(void* p)
{
	S_THREAD_PARAM<DNATrie>* params = (S_THREAD_PARAM<DNATrie>*) p;
	int32_t j=params->j;
	CWeightedDegreePositionStringKernel* wd=params->kernel;
	CTrie<DNATrie>* tries=params->tries;
	float64_t* weights=params->weights;
	int32_t length=params->length;
	int32_t max_shift=params->max_shift;
	int32_t* vec=params->vec;
	float64_t* result=params->result;
	float64_t factor=params->factor;
	int32_t* shift=params->shift;
	int32_t* vec_idx=params->vec_idx;

	for (int32_t i=params->start; i<params->end; i++)
	{
		int32_t len=0;
		CStringFeatures<char>* rhs_feat=((CStringFeatures<char>*) wd->get_rhs());
		CAlphabet* alpha=wd->alphabet;

		char* char_vec=rhs_feat->get_feature_vector(vec_idx[i], len);
		for (int32_t k=CMath::max(0,j-max_shift); k<CMath::min(len,j+wd->get_degree()+max_shift); k++)
			vec[k]=alpha->remap_to_bin(char_vec[k]);

		SG_UNREF(rhs_feat);

		result[i] += factor*wd->normalizer->normalize_rhs(tries->compute_by_tree_helper(vec, len, j, j, j, weights, (length!=0)), vec_idx[i]);

		if (wd->get_optimization_type()==SLOWBUTMEMEFFICIENT)
		{
			for (int32_t q=CMath::max(0,j-max_shift); q<CMath::min(len,j+max_shift+1); q++)
			{
				int32_t s=j-q ;
				if ((s>=1) && (s<=shift[q]) && (q+s<len))
				{
					result[i] +=
						wd->normalizer->normalize_rhs(tries->compute_by_tree_helper(vec,
								len, q, q+s, q, weights, (length!=0)),
								vec_idx[i])/(2.0*s);
				}
			}

			for (int32_t s=1; (s<=shift[j]) && (j+s<len); s++)
			{
				result[i] +=
					wd->normalizer->normalize_rhs(tries->compute_by_tree_helper(vec,
								len, j+s, j, j+s, weights, (length!=0)),
								vec_idx[i])/(2.0*s);
			}
		}
	}

	return NULL;
}

void CWeightedDegreePositionStringKernel::compute_batch(
	int32_t num_vec, int32_t* vec_idx, float64_t* result, int32_t num_suppvec,
	int32_t* IDX, float64_t* alphas, float64_t factor)
{
	ASSERT(alphabet);
	ASSERT(alphabet->get_alphabet()==DNA || alphabet->get_alphabet()==RNA);
	ASSERT(position_weights_lhs==NULL);
	ASSERT(position_weights_rhs==NULL);
	ASSERT(rhs);
	ASSERT(num_vec<=rhs->get_num_vectors());
	ASSERT(num_vec>0);
	ASSERT(vec_idx);
	ASSERT(result);
	create_empty_tries();

	int32_t num_feat=((CStringFeatures<char>*) rhs)->get_max_vector_length();
	ASSERT(num_feat>0);
	int32_t num_threads=parallel.get_num_threads();
	ASSERT(num_threads>0);
	int32_t* vec=new int32_t[num_threads*num_feat];

	if (num_threads < 2)
	{
#ifdef WIN32
	   for (int32_t j=0; j<num_feat; j++)
#else
       CSignal::clear_cancel();
	   for (int32_t j=0; j<num_feat && !CSignal::cancel_computations(); j++)
#endif
			{
				init_optimization(num_suppvec, IDX, alphas, j);
				S_THREAD_PARAM<DNATrie> params;
				params.vec=vec;
				params.result=result;
				params.weights=weights;
				params.kernel=this;
				params.tries=&tries;
				params.factor=factor;
				params.j=j;
				params.start=0;
				params.end=num_vec;
				params.length=length;
				params.max_shift=max_shift;
				params.shift=shift;
				params.vec_idx=vec_idx;
				compute_batch_helper((void*) &params);

				SG_PROGRESS(j,0,num_feat);
			}
	}
#ifndef WIN32
	else
	{

		CSignal::clear_cancel();
		for (int32_t j=0; j<num_feat && !CSignal::cancel_computations(); j++)
		{
			init_optimization(num_suppvec, IDX, alphas, j);
			pthread_t threads[num_threads-1];
			S_THREAD_PARAM<DNATrie> params[num_threads];
			int32_t step= num_vec/num_threads;
			int32_t t;

			for (t=0; t<num_threads-1; t++)
			{
				params[t].vec=&vec[num_feat*t];
				params[t].result=result;
				params[t].weights=weights;
				params[t].kernel=this;
				params[t].tries=&tries;
				params[t].factor=factor;
				params[t].j=j;
				params[t].start = t*step;
				params[t].end = (t+1)*step;
				params[t].length=length;
				params[t].max_shift=max_shift;
				params[t].shift=shift;
				params[t].vec_idx=vec_idx;
				pthread_create(&threads[t], NULL, CWeightedDegreePositionStringKernel::compute_batch_helper, (void*)&params[t]);
			}

			params[t].vec=&vec[num_feat*t];
			params[t].result=result;
			params[t].weights=weights;
			params[t].kernel=this;
			params[t].tries=&tries;
			params[t].factor=factor;
			params[t].j=j;
			params[t].start=t*step;
			params[t].end=num_vec;
			params[t].length=length;
			params[t].max_shift=max_shift;
			params[t].shift=shift;
			params[t].vec_idx=vec_idx;
			compute_batch_helper((void*) &params[t]);

			for (t=0; t<num_threads-1; t++)
				pthread_join(threads[t], NULL);
			SG_PROGRESS(j,0,num_feat);
		}
	}
#endif

	delete[] vec;

	//really also free memory as this can be huge on testing especially when
	//using the combined kernel
	create_empty_tries();
}

float64_t* CWeightedDegreePositionStringKernel::compute_scoring(
	int32_t max_degree, int32_t& num_feat, int32_t& num_sym, float64_t* result,
	int32_t num_suppvec, int32_t* IDX, float64_t* alphas)
{
	ASSERT(position_weights_lhs==NULL);
	ASSERT(position_weights_rhs==NULL);

	num_feat=((CStringFeatures<char>*) rhs)->get_max_vector_length();
	ASSERT(num_feat>0);
	ASSERT(alphabet);
	ASSERT(alphabet->get_alphabet()==DNA || alphabet->get_alphabet()==RNA);

	num_sym=4; //for now works only w/ DNA

	ASSERT(max_degree>0);

	// === variables
	int32_t* nofsKmers=new int32_t[max_degree];
	float64_t** C=new float64_t*[max_degree];
	float64_t** L=new float64_t*[max_degree];
	float64_t** R=new float64_t*[max_degree];

	int32_t i;
	int32_t k;

	// --- return table
	int32_t bigtabSize=0;
	for (k=0; k<max_degree; ++k )
	{
		nofsKmers[k]=(int32_t) pow(num_sym, k+1);
		const int32_t tabSize=nofsKmers[k]*num_feat;
		bigtabSize+=tabSize;
	}
	result=new float64_t[bigtabSize];

	// --- auxilliary tables
	int32_t tabOffs=0;
	for( k = 0; k < max_degree; ++k )
	{
		const int32_t tabSize = nofsKmers[k] * num_feat;
		C[k] = &result[tabOffs];
		L[k] = new float64_t[ tabSize ];
		R[k] = new float64_t[ tabSize ];
		tabOffs+=tabSize;
		for(i = 0; i < tabSize; i++ )
		{
			C[k][i] = 0.0;
			L[k][i] = 0.0;
			R[k][i] = 0.0;
		}
	}

	// --- tree parsing info
	float64_t* margFactors=new float64_t[degree];

	int32_t* x = new int32_t[ degree+1 ];
	int32_t* substrs = new int32_t[ degree+1 ];
	// - fill arrays
	margFactors[0] = 1.0;
	substrs[0] = 0;
	for( k=1; k < degree; ++k ) {
		margFactors[k] = 0.25 * margFactors[k-1];
		substrs[k] = -1;
	}
	substrs[degree] = -1;
	// - fill struct
	struct TreeParseInfo info;
	info.num_sym = num_sym;
	info.num_feat = num_feat;
	info.p = -1;
	info.k = -1;
	info.nofsKmers = nofsKmers;
	info.margFactors = margFactors;
	info.x = x;
	info.substrs = substrs;
	info.y0 = 0;
	info.C_k = NULL;
	info.L_k = NULL;
	info.R_k = NULL;

	// === main loop
	i = 0; // total progress
	for( k = 0; k < max_degree; ++k )
	{
		const int32_t nofKmers = nofsKmers[ k ];
		info.C_k = C[k];
		info.L_k = L[k];
		info.R_k = R[k];

		// --- run over all trees
		for(int32_t p = 0; p < num_feat; ++p )
		{
			init_optimization( num_suppvec, IDX, alphas, p );
			int32_t tree = p ;
			for(int32_t j = 0; j < degree+1; j++ ) {
				x[j] = -1;
			}
			tries.traverse( tree, p, info, 0, x, k );
			SG_PROGRESS(i++,0,num_feat*max_degree);
		}

		// --- add partial overlap scores
		if( k > 0 ) {
			const int32_t j = k - 1;
			const int32_t nofJmers = (int32_t) pow( num_sym, j+1 );
			for(int32_t p = 0; p < num_feat; ++p ) {
				const int32_t offsetJ = nofJmers * p;
				const int32_t offsetJ1 = nofJmers * (p+1);
				const int32_t offsetK = nofKmers * p;
				int32_t y;
				int32_t sym;
				for( y = 0; y < nofJmers; ++y ) {
					for( sym = 0; sym < num_sym; ++sym ) {
						const int32_t y_sym = num_sym*y + sym;
						const int32_t sym_y = nofJmers*sym + y;
						ASSERT(0<=y_sym && y_sym<nofKmers);
						ASSERT(0<=sym_y && sym_y<nofKmers);
						C[k][ y_sym + offsetK ] += L[j][ y + offsetJ ];
						if( p < num_feat-1 ) {
							C[k][ sym_y + offsetK ] += R[j][ y + offsetJ1 ];
						}
					}
				}
			}
		}
		//   if( k > 1 )
		//     j = k-1
		//     for all positions p
		//       for all j-mers y
		//          for n in {A,C,G,T}
		//            C_k[ p, [y,n] ] += L_j[ p, y ]
		//            C_k[ p, [n,y] ] += R_j[ p+1, y ]
		//          end;
		//       end;
		//     end;
		//   end;
	}

	// === return a vector
	num_feat=1;
	num_sym = bigtabSize;
	// --- clean up
	delete[] nofsKmers;
	delete[] margFactors;
	delete[] substrs;
	delete[] x;
	delete[] C;
	for( k = 0; k < max_degree; ++k ) {
		delete[] L[k];
		delete[] R[k];
	}
	delete[] L;
	delete[] R;
	return result;
}

char* CWeightedDegreePositionStringKernel::compute_consensus(
	int32_t &num_feat, int32_t num_suppvec, int32_t* IDX, float64_t* alphas)
{
	ASSERT(position_weights_lhs==NULL);
	ASSERT(position_weights_rhs==NULL);
	//only works for order <= 32
	ASSERT(degree<=32);
	ASSERT(!tries.get_use_compact_terminal_nodes());
	num_feat=((CStringFeatures<char>*) rhs)->get_max_vector_length();
	ASSERT(num_feat>0);
	ASSERT(alphabet);
	ASSERT(alphabet->get_alphabet()==DNA || alphabet->get_alphabet()==RNA);

	//consensus
	char* result=new char[num_feat];

	//backtracking and scoring table
	int32_t num_tables=CMath::max(1,num_feat-degree+1);
	CDynamicArray<ConsensusEntry>** table=new CDynamicArray<ConsensusEntry>*[num_tables];

	for (int32_t i=0; i<num_tables; i++)
		table[i]=new CDynamicArray<ConsensusEntry>(num_suppvec/10);

	//compute consensus via dynamic programming
	for (int32_t i=0; i<num_tables; i++)
	{
		bool cumulative=false;

		if (i<num_tables-1)
			init_optimization(num_suppvec, IDX, alphas, i);
		else
		{
			init_optimization(num_suppvec, IDX, alphas, i, num_feat-1);
			cumulative=true;
		}

		if (i==0)
			tries.fill_backtracking_table(i, NULL, table[i], cumulative, weights);
		else
			tries.fill_backtracking_table(i, table[i-1], table[i], cumulative, weights);

		SG_PROGRESS(i,0,num_feat);
	}


	//int32_t n=table[0]->get_num_elements();

	//for (int32_t i=0; i<n; i++)
	//{
	//	ConsensusEntry e= table[0]->get_element(i);
	//	SG_PRint32_t("first: str:0%0llx sc:%f bt:%d\n",e.string,e.score,e.bt);
	//}

	//n=table[num_tables-1]->get_num_elements();
	//for (int32_t i=0; i<n; i++)
	//{
	//	ConsensusEntry e= table[num_tables-1]->get_element(i);
	//	SG_PRint32_t("last: str:0%0llx sc:%f bt:%d\n",e.string,e.score,e.bt);
	//}
	//n=table[num_tables-2]->get_num_elements();
	//for (int32_t i=0; i<n; i++)
	//{
	//	ConsensusEntry e= table[num_tables-2]->get_element(i);
	//	SG_PRINT("second last: str:0%0llx sc:%f bt:%d\n",e.string,e.score,e.bt);
	//}

	const char* acgt="ACGT";

	//backtracking start
	int32_t max_idx=-1;
	float32_t max_score=0;
	int32_t num_elements=table[num_tables-1]->get_num_elements();

	for (int32_t i=0; i<num_elements; i++)
	{
		float64_t sc=table[num_tables-1]->get_element(i).score;
		if (sc>max_score || max_idx==-1)
		{
			max_idx=i;
			max_score=sc;
		}
	}
	uint64_t endstr=table[num_tables-1]->get_element(max_idx).string;

	SG_INFO("max_idx:%d num_el:%d num_feat:%d num_tables:%d max_score:%f\n", max_idx, num_elements, num_feat, num_tables, max_score);

	for (int32_t i=0; i<degree; i++)
		result[num_feat-1-i]=acgt[(endstr >> (2*i)) & 3];

	if (num_tables>1)
	{
		for (int32_t i=num_tables-1; i>=0; i--)
		{
			//SG_PRINT("max_idx: %d, i:%d\n", max_idx, i);
			result[i]=acgt[table[i]->get_element(max_idx).string >> (2*(degree-1)) & 3];
			max_idx=table[i]->get_element(max_idx).bt;
		}
	}

	//for (int32_t t=0; t<num_tables; t++)
	//{
	//	n=table[t]->get_num_elements();
	//	for (int32_t i=0; i<n; i++)
	//	{
	//		ConsensusEntry e= table[t]->get_element(i);
	//		SG_PRINT("table[%d,%d]: str:0%0llx sc:%+f bt:%d\n",t,i, e.string,e.score,e.bt);
	//	}
	//}

	for (int32_t i=0; i<num_tables; i++)
		delete table[i];

	delete[] table;
	return result;
}


float64_t* CWeightedDegreePositionStringKernel::extract_w(
	int32_t max_degree, int32_t& num_feat, int32_t& num_sym,
	float64_t* w_result, int32_t num_suppvec, int32_t* IDX, float64_t* alphas)
{
  delete_optimization();
  use_poim_tries=true;
  poim_tries.delete_trees(false);

  // === check
  ASSERT(position_weights_lhs==NULL);
  ASSERT(position_weights_rhs==NULL);
  num_feat=((CStringFeatures<char>*) rhs)->get_max_vector_length();
  ASSERT(num_feat>0);
  ASSERT(alphabet->get_alphabet()==DNA);
  ASSERT(max_degree>0);

  // === general variables
  static const int32_t NUM_SYMS = poim_tries.NUM_SYMS;
  const int32_t seqLen = num_feat;
  float64_t** subs;
  int32_t i;
  int32_t k;
  //int32_t y;

  // === init tables "subs" for substring scores / POIMs
  // --- compute table sizes
  int32_t* offsets;
  int32_t offset;
  offsets = new int32_t[ max_degree ];
  offset = 0;
  for( k = 0; k < max_degree; ++k ) {
    offsets[k] = offset;
    const int32_t nofsKmers = (int32_t) pow( NUM_SYMS, k+1 );
    const int32_t tabSize = nofsKmers * seqLen;
    offset += tabSize;
  }
  // --- allocate memory
  const int32_t bigTabSize = offset;
  w_result=new float64_t[bigTabSize];
  for (i=0; i<bigTabSize; ++i)
    w_result[i]=0;

  // --- set pointers for tables
  subs = new float64_t*[ max_degree ];
  ASSERT( subs != NULL );
  for( k = 0; k < max_degree; ++k ) {
    subs[k] = &w_result[ offsets[k] ];
  }
  delete[] offsets;

  // === init trees; extract "w"
  init_optimization( num_suppvec, IDX, alphas, -1);
  poim_tries.POIMs_extract_W( subs, max_degree );

  // === clean; return "subs" as vector
  delete[] subs;
  num_feat = 1;
  num_sym = bigTabSize;
  use_poim_tries=false;
  poim_tries.delete_trees(false);
  return w_result;
}

float64_t* CWeightedDegreePositionStringKernel::compute_POIM(
	int32_t max_degree, int32_t& num_feat, int32_t& num_sym,
	float64_t* poim_result, int32_t num_suppvec, int32_t* IDX,
	float64_t* alphas, float64_t* distrib )
{
  delete_optimization();
  use_poim_tries=true;
  poim_tries.delete_trees(false);

  // === check
  ASSERT(position_weights_lhs==NULL);
  ASSERT(position_weights_rhs==NULL);
  num_feat=((CStringFeatures<char>*) rhs)->get_max_vector_length();
  ASSERT(num_feat>0);
  ASSERT(alphabet->get_alphabet()==DNA);
  ASSERT(max_degree!=0);
  ASSERT(distrib);

  // === general variables
  static const int32_t NUM_SYMS = poim_tries.NUM_SYMS;
  const int32_t seqLen = num_feat;
  float64_t** subs;
  int32_t i;
  int32_t k;

  // === DEBUGGING mode
  //
  // Activated if "max_degree" < 0.
  // Allows to output selected partial score.
  //
  // |max_degree| mod 4
  //   0: substring
  //   1: superstring
  //   2: left overlap
  //   3: right overlap
  //
  const int32_t debug = ( max_degree < 0 ) ? ( abs(max_degree) % 4 + 1 ) : 0;
  if( debug ) {
    max_degree = abs(max_degree) / 4;
    switch( debug ) {
    case 1: {
      printf( "POIM DEBUGGING: substring only (max order=%d)\n", max_degree );
      break;
    }
    case 2: {
      printf( "POIM DEBUGGING: superstring only (max order=%d)\n", max_degree );
      break;
    }
    case 3: {
      printf( "POIM DEBUGGING: left overlap only (max order=%d)\n", max_degree );
      break;
    }
    case 4: {
      printf( "POIM DEBUGGING: right overlap only (max order=%d)\n", max_degree );
      break;
    }
    default: {
      printf( "POIM DEBUGGING: something is wrong (max order=%d)\n", max_degree );
      ASSERT(0);
      break;
    }
    }
  }

  // --- compute table sizes
  int32_t* offsets;
  int32_t offset;
  offsets = new int32_t[ max_degree ];
  offset = 0;
  for( k = 0; k < max_degree; ++k ) {
    offsets[k] = offset;
    const int32_t nofsKmers = (int32_t) pow( NUM_SYMS, k+1 );
    const int32_t tabSize = nofsKmers * seqLen;
    offset += tabSize;
  }
  // --- allocate memory
  const int32_t bigTabSize=offset;
  poim_result=new float64_t[bigTabSize];
  for (i=0; i<bigTabSize; ++i )
    poim_result[i]=0;

  // --- set pointers for tables
  subs=new float64_t*[max_degree];
  for (k=0; k<max_degree; ++k)
    subs[k]=&poim_result[offsets[k]];

  delete[] offsets;

  // === init trees; precalc S, L and R
  init_optimization( num_suppvec, IDX, alphas, -1);
  poim_tries.POIMs_precalc_SLR( distrib );

  // === compute substring scores
  if( debug==0 || debug==1 ) {
    poim_tries.POIMs_extract_W( subs, max_degree );
    for( k = 1; k < max_degree; ++k ) {
      const int32_t nofKmers2 = ( k > 1 ) ? (int32_t) pow(NUM_SYMS,k-1) : 0;
      const int32_t nofKmers1 = (int32_t) pow( NUM_SYMS, k );
      const int32_t nofKmers0 = nofKmers1 * NUM_SYMS;
      for( i = 0; i < seqLen; ++i ) {
	float64_t* const subs_k2i1 = ( k>1 && i<seqLen-1 ) ? &subs[k-2][(i+1)*nofKmers2] : NULL;
	float64_t* const subs_k1i1 = ( i < seqLen-1 ) ? &subs[k-1][(i+1)*nofKmers1] : NULL;
	float64_t* const subs_k1i0 = & subs[ k-1 ][ i*nofKmers1 ];
	float64_t* const subs_k0i  = & subs[ k-0 ][ i*nofKmers0 ];
	int32_t y0;
	for( y0 = 0; y0 < nofKmers0; ++y0 ) {
	  const int32_t y1l = y0 / NUM_SYMS;
	  const int32_t y1r = y0 % nofKmers1;
	  const int32_t y2 = y1r / NUM_SYMS;
	  subs_k0i[ y0 ] += subs_k1i0[ y1l ];
	  if( i < seqLen-1 ) {
	    subs_k0i[ y0 ] += subs_k1i1[ y1r ];
	    if( k > 1 ) {
	      subs_k0i[ y0 ] -= subs_k2i1[ y2 ];
	    }
	  }
	}
      }
    }
  }

  // === compute POIMs
  poim_tries.POIMs_add_SLR( subs, max_degree, debug );

  // === clean; return "subs" as vector
  delete[] subs;
  num_feat = 1;
  num_sym = bigTabSize;

  use_poim_tries=false;
  poim_tries.delete_trees(false);
  
  return poim_result;
}


void CWeightedDegreePositionStringKernel::prepare_POIM2(
	float64_t* distrib, int32_t num_sym, int32_t num_feat)
{
	free(m_poim_distrib);
	m_poim_distrib=(float64_t*)malloc(num_sym*num_feat*sizeof(float64_t));
	ASSERT(m_poim_distrib);

	memcpy(m_poim_distrib, distrib, num_sym*num_feat*sizeof(float64_t));
	m_poim_num_sym=num_sym;
	m_poim_num_feat=num_feat;
}

void CWeightedDegreePositionStringKernel::compute_POIM2(
	int32_t max_degree, CSVM* svm)
{
	ASSERT(svm);
	int32_t num_suppvec=svm->get_num_support_vectors();
	int32_t* sv_idx=new int32_t[num_suppvec];
	float64_t* sv_weight=new float64_t[num_suppvec];

	for (int32_t i=0; i<num_suppvec; i++)
	{
		sv_idx[i]=svm->get_support_vector(i);
		sv_weight[i]=svm->get_alpha(i);
	}
	
	if ((max_degree < 1) || (max_degree > 12))
	{
		//SG_WARNING( "max_degree out of range 1..12 (%d).\n", max_degree);
		SG_WARNING( "max_degree out of range 1..12 (%d). setting to 1.\n", max_degree);
		max_degree=1;
	}
	
	int32_t num_feat = m_poim_num_feat;
	int32_t num_sym = m_poim_num_sym;
	free(m_poim);

	m_poim = compute_POIM(max_degree, num_feat, num_sym, NULL,	num_suppvec, sv_idx, 
						  sv_weight, m_poim_distrib);

	ASSERT(num_feat==1);
	m_poim_result_len=num_sym;
	
	delete[] sv_weight;
	delete[] sv_idx;
}

void CWeightedDegreePositionStringKernel::get_POIM2(
	float64_t** poim, int32_t* result_len)
{
	*poim=(float64_t*) malloc(m_poim_result_len*sizeof(float64_t));
	ASSERT(*poim);
	memcpy(*poim, m_poim, m_poim_result_len*sizeof(float64_t)) ;
	*result_len=m_poim_result_len ;
}

void CWeightedDegreePositionStringKernel::cleanup_POIM2()
{
	free(m_poim) ;
	m_poim=NULL ;
	free(m_poim_distrib) ;
	m_poim_distrib=NULL ;
	m_poim_num_sym=0 ;
	m_poim_num_sym=0 ;
	m_poim_result_len=0 ;
}


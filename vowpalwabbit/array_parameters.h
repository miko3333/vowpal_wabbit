#pragma once
#include <string.h>
#include <unordered_map>
#ifndef _WIN32
#include <sys/mman.h>
#endif

// It appears that on OSX MAP_ANONYMOUS is mapped to MAP_ANON
// https://github.com/leftmike/foment/issues/4
#ifdef __APPLE__
#define MAP_ANONYMOUS MAP_ANON
#endif

typedef float weight;

class dense_parameters;
class sparse_parameters;
typedef std::unordered_map<uint64_t, weight*> weight_map;

class weight_iterator_iterator
{
private:
	weight* _cur;
public:
	weight_iterator_iterator(weight* cur)
		: _cur(cur)
	{ }
	
	weight& operator*() { return *_cur; }

	weight_iterator_iterator& operator++()
	{
		++_cur;
		return *this;
	}

	weight_iterator_iterator operator+(size_t index) { return weight_iterator_iterator(_cur + index); }

	weight_iterator_iterator& operator+=(size_t index)
	{
		_cur += index;
		return *this;
	}

	bool operator==(const weight_iterator_iterator& rhs) const { return _cur == rhs._cur; }
	bool operator!=(const weight_iterator_iterator& rhs) const { return _cur != rhs._cur; }

};

template <typename T>
class dense_iterator
{
private:
	T* _current;
	T* _begin;
	uint32_t _stride;

public:
	typedef std::forward_iterator_tag iterator_category;
	typedef T value_type;
	typedef ptrdiff_t difference_type;
	typedef  T* pointer;
	typedef  T& reference;

	typedef weight_iterator_iterator w_iter;
	
	dense_iterator(T* current, T* begin, uint32_t stride)
		: _current(current), _begin(begin), _stride(stride)
	{ }

	T& operator*() { return *_current; }

	size_t index() { return _current - _begin; }
	
	dense_iterator& operator++()
	{
		_current += _stride;
		return *this;
	}

	bool operator==(const dense_iterator& rhs) const { return _current == rhs._current; }
	bool operator!=(const dense_iterator& rhs) const { return _current != rhs._current; }

	//to iterate within a bucket
	w_iter begin() { return w_iter(_current); }
	w_iter end() { return w_iter(_current + _stride); }
	w_iter end(size_t offset) { return w_iter(_current + offset); }
};

class dense_parameters 
{
private:
	weight* _begin;
	uint64_t _weight_mask;  // (stride*(1 << num_bits) -1)
	uint32_t _stride_shift;
	uint32_t _stride;
	bool _seeded; // whether the instance is sharing model state with others

 public:
	typedef dense_iterator<weight> iterator;
	typedef dense_iterator<const weight> const_iterator;
 dense_parameters(size_t length, uint32_t stride_shift=0)
   : _begin(calloc_mergable_or_throw<weight>(length << stride_shift)),
	  _weight_mask((length << stride_shift) - 1),	
	  _stride_shift(stride_shift),
	  _stride(1 << stride_shift),
	  _seeded(false)
	    { }
	
 dense_parameters()
	 : _begin(nullptr), _weight_mask(0), _stride_shift(0),_stride(1), _seeded(false)
	  {}
	
	bool not_null() { return (_weight_mask > 0 && _begin != nullptr);}

	dense_parameters(const dense_parameters &other) { shallow_copy(other); }
	dense_parameters(dense_parameters &&) = delete;

	weight* first() { return _begin; } //TODO: Temporary fix for allreduce.
	
	//iterator with stride 
	iterator begin() { return iterator(_begin, _begin, _stride); }
	iterator end() { return iterator(_begin + _weight_mask + 1, _begin, _stride); }

	//const iterator
	const_iterator cbegin() { return const_iterator(_begin, _begin,  _stride); }
	const_iterator cend() { return const_iterator(_begin + _weight_mask + 1, _begin, _stride); }

	inline weight& operator[](size_t i) const { return _begin[i & _weight_mask]; }
	void shallow_copy(const dense_parameters& input)
	{ _begin = input._begin;
	  _weight_mask = input._weight_mask;
	  _stride_shift = input._stride_shift;
	  _seeded = true;
	}

	inline weight& strided_index(size_t index){ return operator[](index << _stride_shift);}

	template<class R, void(*T)(iterator&, R&)> void set_default(R& info)
	{
	  iterator iter = begin();
	  for (size_t i = 0; iter != end(); ++iter, i += _stride)
	    T(iter, info);
	}

	template<void(*T)(iterator&)> void set_default()
	{ 
	  iterator iter = begin();
	  for (size_t i = 0; iter != end(); ++iter, i += _stride)
	    T(iter);
	}

	void set_zero(size_t offset)
	{
		for (iterator iter = begin(); iter != end(); ++iter)
			(&(*iter))[offset] = 0;
	}
	
	uint64_t mask()	{ return _weight_mask;	}

	uint64_t seeded() { return _seeded; }

	uint32_t stride() { return _stride; }
	
	uint32_t stride_shift() { return _stride_shift; }		

	void stride_shift(uint32_t stride_shift) { _stride_shift = stride_shift; }
	
	#ifndef _WIN32
	void share(size_t length)
	{
	  float* shared_weights = (float*)mmap(0, (length << _stride_shift) * sizeof(float),
			                  PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
          size_t float_count = length << _stride_shift;
      	  weight* dest = shared_weights;
		  memcpy(dest, _begin, float_count*sizeof(float));
      	  free(_begin);
      	  _begin = dest;
	}
	#endif
	
	~dense_parameters()
	{  if (_begin != nullptr && !_seeded)  // don't free weight vector if it is shared with another instance
	   {  free(_begin);
	      _begin = nullptr;
	   }
	}
};

template <typename T>
class sparse_iterator
{
private:
	weight_map::iterator _iter;
	uint32_t _stride;

public:
	typedef std::forward_iterator_tag iterator_category;
	typedef T value_type;
	typedef ptrdiff_t difference_type;
	typedef  T* pointer;
	typedef  T& reference;

	typedef weight_iterator_iterator w_iter;

	sparse_iterator(weight_map::iterator& iter, uint32_t stride)
		: _iter(iter), _stride(stride)
	{ }

	sparse_iterator& operator=(const sparse_iterator& other)
	{
		_iter = other._iter;
		_stride = other._stride;
		return *this;

	}
	uint64_t index() { return _iter->first; }

	T& operator*() { return *(_iter->second); } 

	sparse_iterator& operator++()
	{  
		_iter++;
		return *this;
	}

	bool operator==(const sparse_iterator& rhs) const { return _iter == rhs._iter; }
	bool operator!=(const sparse_iterator& rhs) const { return _iter != rhs._iter; }

	//to iterate within a bucket
	w_iter begin() { return w_iter(_iter->second);}
	w_iter end() { return w_iter(_iter->second + _stride); }
	w_iter end(size_t offset) { return w_iter(_iter->second + offset);}
};


class sparse_parameters
{
private:
	weight_map _map;
	uint64_t _weight_mask;  // (stride*(1 << num_bits) -1)
	uint32_t _stride_shift;
	uint32_t _stride;
	bool _seeded; // whether the instance is sharing model state with others
	bool _delete;
	void* default_data;
public:
	typedef sparse_iterator<weight> iterator;
	typedef sparse_iterator<const weight> const_iterator;
 private:
	void(*fun)(iterator&, void*);
 public:

	sparse_parameters(size_t length, uint32_t stride_shift = 0)
		: _map(),
		_weight_mask((length << stride_shift) - 1),
		_stride_shift(stride_shift),
		_stride(1 << _stride_shift),
		_seeded(false), _delete(false),
		fun(nullptr)
	{}

	sparse_parameters()
		: _map(), _weight_mask(0), _stride_shift(0), _stride(1), _seeded(false), _delete(false), fun(nullptr)
	{}

	bool not_null() { return (_weight_mask > 0 && !_map.empty()); }

	sparse_parameters(const sparse_parameters &other) { shallow_copy(other); }
	sparse_parameters(sparse_parameters &&) = delete;

	weight* first() { throw 1; } //TODO: Throw better exceptions. Allreduce currently not supported in sparse.

	//iterator with stride 
	iterator begin() { weight_map::iterator i = _map.begin(); return iterator(i, _stride); }
	iterator end() { weight_map::iterator i = _map.end(); return iterator(i, _stride); }

	//const iterator
	const_iterator cbegin() { weight_map::iterator i = _map.begin(); return const_iterator(i,  _stride); }
	const_iterator cend() { weight_map::iterator i = _map.begin(); return const_iterator(i, _stride); }
	
	inline weight& operator[](size_t i)
	{   uint64_t index = i & _weight_mask;
		weight_map::iterator iter = _map.find(index);
		if (iter == _map.end()) 
		  {     _map.insert(std::make_pair(index, calloc_mergable_or_throw<weight>(_stride)));
			iter = _map.find(index);
			if (fun != nullptr)
			  {
			    iterator i(iter,_stride);
			    fun(i, default_data);
			  }
			iter = _map.find(index);
		}
		return *(iter->second);
	}

	inline weight& strided_index(size_t index) { return operator[](index << _stride_shift); }
	
	void shallow_copy(const sparse_parameters& input)
	{
		_map = input._map;
		_weight_mask = input._weight_mask;
		_stride_shift = input._stride_shift;
		_seeded = true;
	}

	template<class R, void(*T)(iterator&, R&)> void set_default(R& info)
	{
	  R& new_R = calloc_or_throw<R>();
	  new_R = info;
	  default_data = &new_R;
	  fun = (void(*)(iterator&, void*))T;
	}

	template<void(*T)(iterator&)> void set_default() { fun = (void(*)(iterator&, void*))T; }

	void set_zero(size_t offset)
	{
		for (weight_map::iterator iter = _map.begin(); iter != _map.end(); ++iter)
			(&(*(iter->second)))[offset] = 0;
	}

	uint64_t mask()	{ return _weight_mask; }

	uint64_t seeded() { return _seeded; }

	uint32_t stride() { return _stride; }

	uint32_t stride_shift()	{ return _stride_shift; }

	void stride_shift(uint32_t stride_shift) { _stride_shift = stride_shift; }

#ifndef _WIN32
	void share(size_t length)
	{throw 1; //TODO: add better exceptions
	}
#endif

	~sparse_parameters()
	{if (!_delete && !_seeded)  // don't free weight vector if it is shared with another instance
		{
		 _map.clear();
		 _delete = true;
		}
	}
};

class parameters {
 public:
  bool sparse;
  dense_parameters dense_weights;
  sparse_parameters sparse_weights;

  inline weight& operator[](size_t i)
  {
    if (sparse)
      return sparse_weights[i];
    else
      return dense_weights[i];
  }

  inline uint32_t stride_shift()
  {
    if (sparse)
      return sparse_weights.stride_shift();
    else
      return dense_weights.stride_shift();
  }

  inline uint32_t stride()
  {
    if (sparse)
      return sparse_weights.stride();
    else
      return dense_weights.stride();
  }

  inline uint64_t mask()
  {
    if (sparse)
      return sparse_weights.mask();
    else
      return dense_weights.mask();
  }

  inline uint64_t seeded()
  {
    if (sparse)
      return sparse_weights.seeded();
    else
      return dense_weights.seeded();
  }

  inline void shallow_copy(const parameters& input)
  {
    if (sparse)
      sparse_weights.shallow_copy(input.sparse_weights);
    else
      dense_weights.shallow_copy(input.dense_weights);
  }

  void set_zero(size_t offset)
  {
    if (sparse)
      sparse_weights.set_zero(offset);
    else
      dense_weights.set_zero(offset);
  }

  void share(size_t length)
  {
    if (sparse)
      sparse_weights.share(length);
    else
      dense_weights.share(length);
  }
};
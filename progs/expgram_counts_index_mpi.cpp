//
//  Copyright(C) 2009-2013 Taro Watanabe <taro.watanabe@nict.go.jp>
//

#include <iostream>
#include <stdexcept>

#include <vector>
#include <queue>
#include <deque>

#include <expgram/NGramCounts.hpp>
#include <expgram/NGramCountsIndexer.hpp>

#include <boost/thread.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/range.hpp>

#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filter/bzip2.hpp>

#include <utils/compress_stream.hpp>
#include <utils/lockfree_queue.hpp>
#include <utils/lockfree_list_queue.hpp>
#include <utils/tempfile.hpp>
#include <utils/resource.hpp>
#include <utils/repository.hpp>
#include <utils/lexical_cast.hpp>
#include <utils/mpi.hpp>
#include <utils/mpi_device.hpp>
#include <utils/mpi_device_bcast.hpp>

typedef expgram::NGramCounts ngram_type;

typedef ngram_type::size_type       size_type;
typedef ngram_type::difference_type difference_type;
typedef ngram_type::path_type       path_type;


typedef ngram_type::count_type      count_type;
typedef ngram_type::vocab_type      vocab_type;
typedef ngram_type::word_type       word_type;
typedef ngram_type::id_type         id_type;

typedef utils::mpi_intercomm intercomm_type;

path_type ngram_file;
path_type output_file;
path_type temporary_dir = "";

path_type prog_name;
std::string host;
std::string hostfile;

int debug = 0;

enum {
  count_tag = 3000,
  file_tag,
  size_tag,
  sync_tag,
};

void index_ngram_mapper_root(intercomm_type& reducer,
			     const path_type& path,
			     ngram_type& ngram);
void index_ngram_mapper_others(intercomm_type& reducer,
			       ngram_type& ngram);

template <typename Stream>
void index_unigram(const path_type& path,
		   const path_type& output,
		   ngram_type& ngram,
		   Stream& os_counts);
template <typename Stream>
void index_ngram_reducer(intercomm_type& mapper, ngram_type& ngram, Stream& os_count);


int getoptions(int argc, char** argv);

int main(int argc, char** argv)
{
  utils::mpi_world mpi_world(argc, argv);
  
  const int mpi_rank = MPI::COMM_WORLD.Get_rank();
  const int mpi_size = MPI::COMM_WORLD.Get_size();
  
  try {
    
    if (MPI::Comm::Get_parent() != MPI::COMM_NULL) {
      // child-prcess...
      // this child-process work as mappers...
      
      utils::mpi_intercomm comm_parent(MPI::Comm::Get_parent());
      
      if (getoptions(argc, argv) != 0) 
	return 1;

      if (! temporary_dir.empty())
	::setenv("TMPDIR_SPEC", temporary_dir.string().data(), 1);
      
      ngram_type ngram;
      
      int unigram_size = 0;
      if (mpi_rank == 0)
	comm_parent.comm.Recv(&unigram_size, 1, MPI::INT, 0, size_tag);
      MPI::COMM_WORLD.Bcast(&unigram_size, 1, MPI::INT, 0);
      
      ngram.index.reserve(mpi_size);
      ngram.index.resize(mpi_size);
      
      ngram.index[mpi_rank].offsets.clear();
      ngram.index[mpi_rank].offsets.push_back(0);
      ngram.index[mpi_rank].offsets.push_back(unigram_size);
      
      // setup vocabulary...
      {
	typedef utils::repository repository_type;
	
	while (! repository_type::exists(output_file))
	  boost::thread::yield();
	repository_type repository(output_file, repository_type::read);
	
	while (! repository_type::exists(repository.path("index")))
	  boost::thread::yield();
	repository_type rep(repository.path("index"), repository_type::read);
	
	while (! vocab_type::exists(rep.path("vocab")))
	  boost::thread::yield();
	
	ngram.index.vocab().open(rep.path("vocab"));
      }

      utils::resource start;
      
      // perform mapping!
      if (mpi_rank == 0)
	index_ngram_mapper_root(comm_parent, ngram_file, ngram);
      else
	index_ngram_mapper_others(comm_parent, ngram);

      utils::resource end;
      
      if (debug && mpi_rank == 0)
	std::cerr << "index counts mapper"
		  << " cpu time:  " << end.cpu_time() - start.cpu_time() 
		  << " user time: " << end.user_time() - start.user_time()
		  << std::endl;
      
    } else {
      std::vector<const char*, std::allocator<const char*> > args;
      args.reserve(argc);
      for (int i = 1; i < argc; ++ i)
	args.push_back(argv[i]);
      args.push_back(0);
      
      // getoptions...
      if (getoptions(argc, argv) != 0) 
	return 1;
      
      if (! temporary_dir.empty())
	::setenv("TMPDIR_SPEC", temporary_dir.string().data(), 1);
      
      if (ngram_file.empty() || ! boost::filesystem::exists(ngram_file))
	throw std::runtime_error(std::string("no ngram file? ") + ngram_file.string());
      if (output_file.empty())
	throw std::runtime_error(std::string("no output?"));
      if (! prog_name.empty() && ! boost::filesystem::exists(prog_name))
	throw std::runtime_error(std::string("no binary? ") + prog_name.string());
      
      // we are reducers..
      const path_type tmp_dir = utils::tempfile::tmp_dir();
      const path_type path_count = utils::tempfile::directory_name(tmp_dir / "expgram.count.XXXXXX");
      utils::tempfile::insert(path_count);
      
      ngram_type ngram;
      
      boost::iostreams::filtering_ostream os_count;
      os_count.push(utils::packed_sink<count_type, std::allocator<count_type> >(path_count));
      
      utils::resource start;
      
      index_unigram(ngram_file, output_file, ngram, os_count);

      utils::resource end;
      
      if (debug && mpi_rank == 0)
	std::cerr << "index unigram"
		  << " cpu time:  " << end.cpu_time() - start.cpu_time() 
		  << " user time: " << end.user_time() - start.user_time()
		  << std::endl;
      
      {
	std::vector<int, std::allocator<int> > error_codes(mpi_size, MPI_SUCCESS);
	
	const std::string name = (boost::filesystem::exists(prog_name) ? prog_name.string() : std::string(argv[0]));
	
	MPI::Info info = MPI::Info::Create();
	
	if (! host.empty())
	  info.Set("host", host.c_str());
	if (! hostfile.empty())
	  info.Set("hostfile", hostfile.c_str());
	
	utils::mpi_intercomm comm_child(MPI::COMM_WORLD.Spawn(name.c_str(), &(*args.begin()), mpi_size, info, 0, &(*error_codes.begin())));
	
	info.Free();
	
	for (size_t i = 0; i != error_codes.size(); ++ i)
	  if (error_codes[i] != MPI_SUCCESS)
	    throw std::runtime_error("one of children failed to launch!");
	
	if (mpi_rank == 0) {
	  const int unigram_size = ngram.index[0].offsets[1];
	  comm_child.comm.Send(&unigram_size, 1, MPI::INT, 0, size_tag);
	}

	utils::resource start;
	
	index_ngram_reducer(comm_child, ngram, os_count);
	
	utils::resource end;
	
	if (debug && mpi_rank == 0)
	  std::cerr << "index counts reducer"
		    << " cpu time:  " << end.cpu_time() - start.cpu_time() 
		    << " user time: " << end.user_time() - start.user_time()
		    << std::endl;
      }
      
      // perform indexing and open
      os_count.pop();
      
      while (! ngram_type::shard_data_type::count_set_type::exists(path_count))
	boost::thread::yield();
      
      utils::tempfile::permission(path_count);
      
      ngram.counts[mpi_rank].counts.open(path_count);
      
      // final dump...
      ngram.write_shard(output_file, mpi_rank);
    }
  }
  catch (const std::exception& err) {
    std::cerr << "error: " << err.what() << std::endl;
    MPI::COMM_WORLD.Abort(1);
    return 1;
  }
  return 0;
}

inline
int loop_sleep(bool found, int non_found_iter)
{
  if (! found) {
    boost::thread::yield();
    ++ non_found_iter;
  } else
    non_found_iter = 0;
  
  if (non_found_iter >= 50) {
    struct timespec tm;
    tm.tv_sec = 0;
    tm.tv_nsec = 2000001;
    nanosleep(&tm, NULL);
    
    non_found_iter = 0;
  }
  return non_found_iter;
}
  
inline
word_type escape_word(const utils::piece& __word)
{
  static const std::string& __BOS = static_cast<const std::string&>(vocab_type::BOS);
  static const std::string& __EOS = static_cast<const std::string&>(vocab_type::EOS);
  static const std::string& __UNK = static_cast<const std::string&>(vocab_type::UNK);
  
  const utils::ipiece word(__word);
      
  if (word == __BOS)
    return vocab_type::BOS;
  else if (word == __EOS)
    return vocab_type::EOS;
  else if (word == __UNK)
    return vocab_type::UNK;
  else
    return __word;
}

inline
boost::filesystem::path compressed_filename(const boost::filesystem::path& path)
{
  typedef boost::filesystem::path path_type;
    
  if (boost::filesystem::exists(path))
    return path;
    
  const std::string extension = path.extension().string();
    
  if (extension == ".gz" || extension == ".bz2") {
    // try strip and add new extention..
    const path_type path_gz  = path.parent_path() / (path.stem().string() + ".gz");
    const path_type path_bz2 = path.parent_path() / (path.stem().string() + ".bz2");
    
    if (boost::filesystem::exists(path_gz))
      return path_gz;
    else if (boost::filesystem::exists(path_bz2))
      return path_bz2;
  } else {
    // try add .gz....
    const path_type path_gz  = path.parent_path() / (path.filename().string() + ".gz");
    const path_type path_bz2 = path.parent_path() / (path.filename().string() + ".bz2");
    
    if (boost::filesystem::exists(path_gz))
      return path_gz;
    else if (boost::filesystem::exists(path_bz2))
      return path_bz2;
  }
    
  return path;
}

template <typename Stream>
void index_unigram(const path_type& path, const path_type& output, ngram_type& ngram, Stream& os_counts)
{
  typedef utils::repository repository_type;
  
  typedef ngram_type::count_type count_type;
  typedef ngram_type::id_type    id_type;
  
  typedef std::vector<utils::piece, std::allocator<utils::piece> > tokens_type;
  typedef boost::tokenizer<utils::space_separator, utils::piece::const_iterator, utils::piece> tokenizer_type;
  
  const int mpi_rank = MPI::COMM_WORLD.Get_rank();
  const int mpi_size = MPI::COMM_WORLD.Get_size();
  
  ngram.index.reserve(mpi_size);
  ngram.counts.reserve(mpi_size);
  
  ngram.index.resize(mpi_size);
  ngram.counts.resize(mpi_size);
  
  if (mpi_rank == 0) {
    typedef std::vector<word_type, std::allocator<word_type> > word_set_type;

    if (debug)
      std::cerr << "order: " << 1 << std::endl;
    
    const path_type unigram_dir       = path / "1gms";
    const path_type vocab_file        = compressed_filename(unigram_dir / "vocab.gz");
    const path_type vocab_sorted_file = compressed_filename(unigram_dir / "vocab_cs.gz");
    
    utils::compress_istream is(vocab_sorted_file, 1024 * 1024);
    
    word_set_type words;
    
    id_type word_id = 0;

    std::string line;
    tokens_type tokens;
    while (std::getline(is, line)) {
      utils::piece line_piece(line);
      tokenizer_type tokenizer(line_piece);
      tokens.clear();
      tokens.insert(tokens.end(), tokenizer.begin(), tokenizer.end());
      
      if (tokens.size() != 2) continue;
      
      const word_type word = escape_word(tokens.front());
      const count_type count = utils::lexical_cast<count_type>(tokens.back());
      
      os_counts.write((char*) &count, sizeof(count_type));
      
      words.push_back(word);
      
      ++ word_id;
    }
    
    const path_type path_vocab = utils::tempfile::directory_name(utils::tempfile::tmp_dir() / "expgram.vocab.XXXXXX");
    utils::tempfile::insert(path_vocab);
    
    vocab_type& vocab = ngram.index.vocab();
    vocab.open(path_vocab, words.size() >> 1);
    
    word_set_type::const_iterator witer_end = words.end();
    for (word_set_type::const_iterator witer = words.begin(); witer != witer_end; ++ witer)
      vocab.insert(*witer);
    
    words.clear();
    word_set_type(words).swap(words);
    
    vocab.close();
    ::sync();

    while (! vocab_type::exists(path_vocab))
      boost::thread::yield();
    
    utils::tempfile::permission(path_vocab);
    
    vocab.open(path_vocab);
    
    // prepare directory structures...
    // write-prepare will create directories, and dump vocabulary....
    ngram.write_prepare(output);
    
    int unigram_size = word_id;
    MPI::COMM_WORLD.Bcast(&unigram_size, 1, MPI::INT, 0);
    
    if (debug)
      std::cerr << "\t1-gram size: " << unigram_size << std::endl;
    
    ngram.index[0].offsets.clear();
    ngram.index[0].offsets.push_back(0);
    ngram.index[0].offsets.push_back(unigram_size);
    
    ngram.counts[0].offset = 0;
  } else {
    int unigram_size = 0;
    MPI::COMM_WORLD.Bcast(&unigram_size, 1, MPI::INT, 0);
    
    ngram.index[mpi_rank].offsets.clear();
    ngram.index[mpi_rank].offsets.push_back(0);
    ngram.index[mpi_rank].offsets.push_back(unigram_size);
    
    ngram.counts[mpi_rank].offset = unigram_size;
    
    while (! repository_type::exists(output))
      boost::thread::yield();
    repository_type repository(output, repository_type::read);
    
    while (! repository_type::exists(repository.path("index")))
      boost::thread::yield();
    repository_type rep(repository.path("index"), repository_type::read);
    
    while (! vocab_type::exists(rep.path("vocab")))
      boost::thread::yield();
    
    ngram.index.vocab().open(rep.path("vocab"));
  }
}


struct VocabMap
{
  typedef std::vector<id_type, std::allocator<id_type> > cache_type;

  VocabMap(vocab_type& __vocab) : vocab(__vocab) {}
  
  id_type operator[](const word_type& word)
  {
    if (word.id() >= cache.size())
      cache.resize(word.id() + 1, id_type(-1));
    if (cache[word.id()] == id_type(-1))
      cache[word.id()] = vocab[word];
    return cache[word.id()];
  }
  
  vocab_type& vocab;
  cache_type  cache;
};


template <typename Tp>
struct greater_ngram
{
  bool operator()(const Tp* x, const Tp* y) const
  {
    return x->first.front().first > y->first.front().first;
  }
      
  bool operator()(const boost::shared_ptr<Tp>& x, const boost::shared_ptr<Tp>& y) const
  {
    return x->first.front().first > y->first.front().first;
  }
};

template <typename NGram, typename VocabMap, typename Context>
int shard_index(const NGram& ngram, VocabMap& vocab_map, const Context& context)
{
  id_type id[2];
  id[0] = vocab_map[escape_word(context[0])];
  id[1] = vocab_map[escape_word(context[1])];
  
  return ngram.index.shard_index(id, id + 2);
}

template <typename PathSet, typename VocabMap>
void index_ngram_mapper(intercomm_type& reducer, const PathSet& paths, ngram_type& ngram, VocabMap& vocab_map)
{
  typedef boost::iostreams::filtering_istream istream_type;
  typedef boost::iostreams::filtering_ostream ostream_type;
  
  typedef utils::mpi_device_source            idevice_type;
  typedef utils::mpi_device_sink              odevice_type;

  typedef boost::shared_ptr<istream_type> istream_ptr_type;
  typedef boost::shared_ptr<ostream_type> ostream_ptr_type;
  
  typedef boost::shared_ptr<idevice_type> idevice_ptr_type;
  typedef boost::shared_ptr<odevice_type> odevice_ptr_type;
  
  typedef std::vector<istream_ptr_type, std::allocator<istream_ptr_type> > istream_ptr_set_type;
  typedef std::vector<ostream_ptr_type, std::allocator<ostream_ptr_type> > ostream_ptr_set_type;
  
  typedef std::vector<idevice_ptr_type, std::allocator<idevice_ptr_type> > idevice_ptr_set_type;
  typedef std::vector<odevice_ptr_type, std::allocator<odevice_ptr_type> > odevice_ptr_set_type;

  typedef std::vector<std::string, std::allocator<std::string> > ngram_context_type;
  
  typedef std::pair<ngram_context_type, count_type>                           context_count_type;
  typedef std::deque<context_count_type, std::allocator<context_count_type> > context_count_set_type;
  
  typedef std::pair<context_count_set_type, istream_type*> context_count_stream_type;
  
  typedef std::vector<context_count_stream_type, std::allocator<context_count_stream_type> > context_count_stream_set_type;
  
  typedef std::vector<context_count_stream_type*, std::allocator<context_count_stream_type*> > pqueue_base_type;
  typedef std::priority_queue<context_count_stream_type*, pqueue_base_type, greater_ngram<context_count_stream_type> > pqueue_type;

  typedef std::vector<utils::piece, std::allocator<utils::piece> > tokens_type;
  typedef boost::tokenizer<utils::space_separator, utils::piece::const_iterator, utils::piece> tokenizer_type;

  const int mpi_rank = MPI::COMM_WORLD.Get_rank();
  const int mpi_size = MPI::COMM_WORLD.Get_size();
  
  ostream_ptr_set_type stream(mpi_size);
  odevice_ptr_set_type device(mpi_size);
  
  for (int rank = 0; rank < mpi_size; ++ rank) {
    stream[rank].reset(new ostream_type());
    device[rank].reset(new odevice_type(reducer.comm, rank, count_tag, 1024 * 1024, false, true));
    stream[rank]->push(boost::iostreams::zlib_compressor());
    stream[rank]->push(*device[rank]);
  }

  pqueue_type pqueue;
  
  istream_ptr_set_type          istreams(paths.size());
  context_count_stream_set_type context_counts(paths.size());
  
  std::string line;
  tokens_type tokens;
  
  // any faster merging...?

  for (size_t i = 0; i != paths.size(); ++ i) {
    istreams[i].reset(new utils::compress_istream(paths[i], 1024 * 1024));
    
    context_count_stream_type* context_stream = &context_counts[i];
    context_stream->second = &(*istreams[i]);
    
    while (context_stream->first.size() < 256 && std::getline(*context_stream->second, line)) {
      utils::piece line_piece(line);
      tokenizer_type tokenizer(line_piece);
      tokens.clear();
      tokens.insert(tokens.end(), tokenizer.begin(), tokenizer.end());
      
      if (tokens.size() < 2) continue;
      
      context_stream->first.push_back(std::make_pair(ngram_context_type(tokens.begin(), tokens.end() - 1),
						     utils::lexical_cast<count_type>(tokens.back())));
    }
    
    if (! context_stream->first.empty())
      pqueue.push(context_stream);
  }

  namespace karma = boost::spirit::karma;
  namespace standard = boost::spirit::standard;
  
  karma::uint_generator<count_type> count_generator;
  
  ngram_context_type context;
  count_type         count = 0;
  
  ngram_context_type prefix_shard;
  int                ngram_shard = 0;
  
  const size_t iteration_mask = (1 << 13) - 1;
  for (size_t iteration = 0; ! pqueue.empty(); ++ iteration) {
    context_count_stream_type* context_stream(pqueue.top());
    pqueue.pop();
    
    if (context != context_stream->first.front().first) {
      if (count > 0) {
	if (context.size() == 2)
	  ngram_shard = shard_index(ngram, vocab_map, context);
	else if (prefix_shard.empty() || ! std::equal(prefix_shard.begin(), prefix_shard.end(), context.begin())) {
	  ngram_shard = shard_index(ngram, vocab_map, context);
	  
	  prefix_shard.clear();
	  prefix_shard.insert(prefix_shard.end(), context.begin(), context.begin() + 2);
	}

	std::ostream_iterator<char> streamiter(*stream[ngram_shard]);
	if (! karma::generate(streamiter, +(standard::string << ' ') << count_generator << '\n', context, count))
	  throw std::runtime_error("generation failed");
	
	//std::copy(context.begin(), context.end(), std::ostream_iterator<std::string>(*stream[ngram_shard], " "));
	//*stream[ngram_shard] << count << '\n';
      }
      
      context.swap(context_stream->first.front().first);
      count = 0;
    }
    
    count += context_stream->first.front().second;
    context_stream->first.pop_front();
    
    if (context_stream->first.empty())
      while (context_stream->first.size() < 256 && std::getline(*(context_stream->second), line)) {
	utils::piece line_piece(line);
	tokenizer_type tokenizer(line_piece);
	tokens.clear();
	tokens.insert(tokens.end(), tokenizer.begin(), tokenizer.end());
	
	if (tokens.size() < 2) continue;
	
	context_stream->first.push_back(std::make_pair(ngram_context_type(tokens.begin(), tokens.end() - 1),
						       utils::lexical_cast<count_type>(tokens.back())));
      }
    
    if (! context_stream->first.empty())
      pqueue.push(context_stream);
    
    if ((iteration & iteration_mask) == iteration_mask && utils::mpi_flush_devices(stream, device))
      boost::thread::yield();
  }

  // final dumping...
  if (count > 0) {
    if (context.size() == 2)
      ngram_shard = shard_index(ngram, vocab_map, context);
    else if (prefix_shard.empty() || ! std::equal(prefix_shard.begin(), prefix_shard.end(), context.begin())) {
      ngram_shard = shard_index(ngram, vocab_map, context);
      
      prefix_shard.clear();
      prefix_shard.insert(prefix_shard.end(), context.begin(), context.begin() + 2);
    }
    
    std::ostream_iterator<char> streamiter(*stream[ngram_shard]);
    if (! karma::generate(streamiter, +(standard::string << ' ') << count_generator << '\n', context, count))
      throw std::runtime_error("generation failed");
    
    //std::copy(context.begin(), context.end(), std::ostream_iterator<std::string>(*stream[ngram_shard], " "));
    //*stream[ngram_shard] << count << '\n';
  }
  
  istreams.clear();
  
  for (int rank = 0; rank < mpi_size; ++ rank) {
    *stream[rank] << '\n';
    stream[rank].reset();
  }
  
  int non_found_iter = 0;
  
  for (;;) {
    if (std::count(device.begin(), device.end(), odevice_ptr_type()) == mpi_size) break;
    
    non_found_iter = loop_sleep(utils::mpi_terminate_devices(stream, device), non_found_iter);
  }
}

void index_ngram_mapper_root(intercomm_type& reducer, const path_type& path, ngram_type& ngram)
{
  typedef boost::iostreams::filtering_ostream ostream_type;
  typedef boost::shared_ptr<ostream_type> ostream_ptr_type;
  typedef std::vector<ostream_ptr_type, std::allocator<ostream_ptr_type> > ostream_ptr_set_type;
  
  typedef std::vector<path_type, std::allocator<path_type> >     path_set_type;
  
  typedef std::vector<utils::piece, std::allocator<utils::piece> > tokens_type;
  typedef boost::tokenizer<utils::space_separator, utils::piece::const_iterator, utils::piece> tokenizer_type;
  
  typedef VocabMap vocab_map_type;
  
  vocab_map_type vocab_map(ngram.index.vocab());
  
  const int mpi_rank = MPI::COMM_WORLD.Get_rank();
  const int mpi_size = MPI::COMM_WORLD.Get_size();
  
  for (int order = 2; /**/; ++ order) {
    std::ostringstream stream_ngram;
    stream_ngram << order << "gms";
    
    std::ostringstream stream_index;
    stream_index << order << "gm.idx";
    
    const path_type ngram_dir = path / stream_ngram.str();
    const path_type index_file = ngram_dir / stream_index.str();

    path_set_type paths_ngram;
    
    if (boost::filesystem::exists(ngram_dir) && boost::filesystem::exists(index_file)) {
      utils::compress_istream is_index(index_file);
      std::string line;
      tokens_type tokens;
      while (std::getline(is_index, line)) {
	utils::piece line_piece(line);
	tokenizer_type tokenizer(line_piece);
	
	tokens.clear();
	tokens.insert(tokens.end(), tokenizer.begin(), tokenizer.end());
	
	if (tokens.empty()) continue;
	
	if (static_cast<int>(tokens.size()) != order + 1)
	  throw std::runtime_error(std::string("invalid google ngram format...") + index_file.string());

	const path_type path_ngram = compressed_filename(ngram_dir / static_cast<std::string>(tokens.front()));
	
	if (! boost::filesystem::exists(path_ngram))
	  throw std::runtime_error(std::string("invalid google ngram format... no file: ") + path_ngram.string());
	
	paths_ngram.push_back(path_ngram);
      }
    }
    
    int count_file_size = paths_ngram.size();
    
    // notify reducers
    reducer.comm.Send(&count_file_size, 1, MPI::INT, 0, size_tag);
    
    // notify mappers
    MPI::COMM_WORLD.Bcast(&count_file_size, 1, MPI::INT, 0);
    
    if (count_file_size == 0) break;
    
    if (debug)
      std::cerr << "mapper order: " << order << " # of files: " << count_file_size << std::endl;
    
    // distribute files...
    ostream_ptr_set_type stream(mpi_size);
    for (int rank = 1; rank < mpi_size; ++ rank) {
      stream[rank].reset(new ostream_type());
      stream[rank]->push(boost::iostreams::zlib_compressor());
      stream[rank]->push(utils::mpi_device_sink(MPI::COMM_WORLD, rank, file_tag, 1024 * 4));
    }
    
    path_set_type paths_map;
    for (size_t i = 0; i < paths_ngram.size(); ++ i) {
      const int rank = i % mpi_size;
      
      if (rank == mpi_rank)
	paths_map.push_back(paths_ngram[i]);
      else
	*stream[rank] << paths_ngram[i].string() << '\n';
    }
    
    for (int rank = 1; rank < mpi_size; ++ rank) {
      *stream[rank] << '\n';
      stream[rank].reset();
    }
    stream.clear();
    
    index_ngram_mapper(reducer, paths_map, ngram, vocab_map);
  }
}

void index_ngram_mapper_others(intercomm_type& reducer, ngram_type& ngram)
{
  typedef std::vector<path_type, std::allocator<path_type> >     path_set_type;

  typedef VocabMap vocab_map_type;
  
  vocab_map_type vocab_map(ngram.index.vocab());
  
  const int mpi_rank = MPI::COMM_WORLD.Get_rank();
  const int mpi_size = MPI::COMM_WORLD.Get_size();
  
  for (int order = 2; /**/; ++ order) {
    int count_file_size = 0;
    MPI::COMM_WORLD.Bcast(&count_file_size, 1, MPI::INT, 0);
    
    if (count_file_size == 0) break;
    
    path_set_type paths_map;
    boost::iostreams::filtering_istream stream;
    stream.push(boost::iostreams::zlib_decompressor());
    stream.push(utils::mpi_device_source(MPI::COMM_WORLD, 0, file_tag, 1024 * 4));
    
    std::string line;
    while (std::getline(stream, line))
      if (! line.empty()) {
	if (! boost::filesystem::exists(line))
	  throw std::runtime_error(std::string("no file? ") + line);
	paths_map.push_back(line);
      }
    
    index_ngram_mapper(reducer, paths_map, ngram, vocab_map);
  }
}

struct IndexNGramMapReduce
{
  typedef std::vector<id_type, std::allocator<id_type> > context_type;
  typedef std::pair<context_type, count_type> context_count_type;
  
  typedef utils::lockfree_list_queue<context_count_type, std::allocator<context_count_type> > queue_type;
  typedef boost::shared_ptr<queue_type>                                                  queue_ptr_type;
  typedef std::vector<queue_ptr_type, std::allocator<queue_ptr_type> >                   queue_ptr_set_type;
  
  typedef boost::thread                                                  thread_type;
  typedef boost::shared_ptr<thread_type>                                 thread_ptr_type;
  typedef std::vector<thread_ptr_type, std::allocator<thread_ptr_type> > thread_ptr_set_type;
  
  typedef boost::iostreams::filtering_ostream                            ostream_type;
};

inline
void swap(IndexNGramMapReduce::context_count_type& x,
	  IndexNGramMapReduce::context_count_type& y)
{
  using namespace std;
  using namespace boost;
  
  x.first.swap(y.first);
  swap(x.second, y.second);
}

struct IndexNGramReducer
{
  typedef IndexNGramMapReduce map_reduce_type;
  
  typedef map_reduce_type::context_type       context_type;
  typedef map_reduce_type::context_count_type context_count_type;
  
  typedef map_reduce_type::queue_type   queue_type;
  typedef map_reduce_type::ostream_type ostream_type;

  typedef expgram::NGramCountsIndexer<ngram_type> indexer_type;
  
  ngram_type&   ngram;
  queue_type&   queue;
  ostream_type& os_count;
  int           shard;
  int           debug;
  
  IndexNGramReducer(ngram_type&           _ngram,
		    queue_type&           _queue,
		    ostream_type&         _os_count,
		    const int             _shard,
		    const int             _debug)
    : ngram(_ngram),
      queue(_queue),
      os_count(_os_count),
      shard(_shard),
      debug(_debug) {}
  
  void operator()()
  {
    typedef std::pair<id_type, count_type> word_count_type;
    typedef std::vector<word_count_type, std::allocator<word_count_type> > word_count_set_type;

    indexer_type indexer;
    
    context_count_type  context_count;
    context_type        prefix;
    word_count_set_type words;
    
    map_reduce_type shard_data;
    
    int order = 0;
    
    while (1) {
      queue.pop_swap(context_count);
      if (context_count.first.empty()) break;
      
      const context_type& context = context_count.first;
      const count_type&   count   = context_count.second;
      
      if (context.size() != prefix.size() + 1 || ! std::equal(prefix.begin(), prefix.end(), context.begin())) {
	
	if (! words.empty()) {
	  indexer(shard, ngram, prefix, words);
	  words.clear();
	  
	  if (static_cast<int>(context.size()) != order)
	    indexer(shard, ngram, os_count, debug);
	}
	
	prefix.clear();
	prefix.insert(prefix.end(), context.begin(), context.end() - 1);
	order = context.size();
      }
      
      words.push_back(std::make_pair(context.back(), count));
    }
    
    // perform final indexing...
    if (! words.empty()) {
      indexer(shard, ngram, prefix, words);
      indexer(shard, ngram, os_count, debug);
    }
  }
};

template <typename Stream, typename VocabMap>
void index_ngram_reducer(intercomm_type& mapper, ngram_type& ngram, Stream& os_count, VocabMap& vocab_map)
{
  typedef boost::iostreams::filtering_istream istream_type;
  typedef utils::mpi_device_source            idevice_type;
  
  typedef boost::shared_ptr<istream_type> istream_ptr_type;
  typedef boost::shared_ptr<idevice_type> idevice_ptr_type;
  
  typedef std::vector<istream_ptr_type, std::allocator<istream_ptr_type> > istream_ptr_set_type;
  typedef std::vector<idevice_ptr_type, std::allocator<idevice_ptr_type> > idevice_ptr_set_type;
  
  typedef std::vector<std::string, std::allocator<std::string> > ngram_context_type;
  
  typedef std::pair<ngram_context_type, count_type>                                       ngram_context_count_type;
  typedef std::deque<ngram_context_count_type, std::allocator<ngram_context_count_type> > context_count_set_type;
  
  typedef std::pair<context_count_set_type, istream_type*> context_count_stream_type;
  typedef boost::shared_ptr<context_count_stream_type>     context_count_stream_ptr_type;
  
  typedef std::vector<context_count_stream_ptr_type, std::allocator<context_count_stream_ptr_type> > pqueue_base_type;
  typedef std::priority_queue<context_count_stream_ptr_type, pqueue_base_type, greater_ngram<context_count_stream_type> > pqueue_type;
  
  typedef std::vector<utils::piece, std::allocator<utils::piece> > tokens_type;
  typedef boost::tokenizer<utils::space_separator, utils::piece::const_iterator, utils::piece> tokenizer_type;
  
  typedef IndexNGramMapReduce map_reduce_type;
  typedef IndexNGramReducer   reducer_type;
  
  typedef map_reduce_type::context_type       context_type;
  typedef map_reduce_type::context_count_type context_count_type;
  typedef map_reduce_type::queue_type         queue_type;
  typedef map_reduce_type::thread_type        thread_type;

  
  const int mpi_rank = MPI::COMM_WORLD.Get_rank();
  const int mpi_size = MPI::COMM_WORLD.Get_size();
  
  queue_type queue(1024 * 64);
  std::auto_ptr<thread_type> thread(new thread_type(reducer_type(ngram, queue, os_count, mpi_rank, debug)));
  
  istream_ptr_set_type stream(mpi_size);
  idevice_ptr_set_type device(mpi_size);

  pqueue_type pqueue;
  
  std::string line;
  tokens_type tokens;
  
  for (int rank = 0; rank < mpi_size; ++ rank) {
    stream[rank].reset(new istream_type());
    device[rank].reset(new idevice_type(mapper.comm, rank, count_tag, 1024 * 1024));
    
    stream[rank]->push(boost::iostreams::zlib_decompressor());
    stream[rank]->push(*device[rank]);
    
    context_count_stream_ptr_type context_stream(new context_count_stream_type());
    context_stream->second = &(*stream[rank]);
    
    while (context_stream->first.size() < 256 && std::getline(*stream[rank], line)) {
      utils::piece line_piece(line);
      tokenizer_type tokenizer(line_piece);
      tokens.clear();
      tokens.insert(tokens.end(), tokenizer.begin(), tokenizer.end());
      
      if (tokens.size() < 2) continue;
      
      context_stream->first.push_back(std::make_pair(ngram_context_type(tokens.begin(), tokens.end() - 1),
						     utils::lexical_cast<count_type>(tokens.back())));
    }
    
    if (! context_stream->first.empty())
      pqueue.push(context_stream);
  }
  
  ngram_context_type  context;
  context_count_type  context_count;
  
  context_count.first.clear();
  context_count.second = 0;
  
  while (! pqueue.empty()) {
    context_count_stream_ptr_type context_stream(pqueue.top());
    pqueue.pop();

    if (context != context_stream->first.front().first) {
      
      if (context_count.second > 0) {
	context_count.first.clear();
	ngram_context_type::const_iterator citer_end = context.end();
	for (ngram_context_type::const_iterator citer = context.begin(); citer != citer_end; ++ citer)
	  context_count.first.push_back(vocab_map[escape_word(*citer)]);
	
	queue.push_swap(context_count);
      }
      
      context.swap(context_stream->first.front().first);
      context_count.second = 0;
    }
    
    context_count.second += context_stream->first.front().second;
    context_stream->first.pop_front();
    
    if (context_stream->first.empty())
      while (context_stream->first.size() < 256 && std::getline(*(context_stream->second), line)) {
	utils::piece line_piece(line);
	tokenizer_type tokenizer(line_piece);
	tokens.clear();
	tokens.insert(tokens.end(), tokenizer.begin(), tokenizer.end());
      
	if (tokens.size() < 2) continue;
      
	context_stream->first.push_back(std::make_pair(ngram_context_type(tokens.begin(), tokens.end() - 1),
						       utils::lexical_cast<count_type>(tokens.back())));
      }
    
    if (! context_stream->first.empty())
      pqueue.push(context_stream);
  }
  
  if (context_count.second > 0) {
    context_count.first.clear();
    ngram_context_type::const_iterator citer_end = context.end();
    for (ngram_context_type::const_iterator citer = context.begin(); citer != citer_end; ++ citer)
      context_count.first.push_back(vocab_map[escape_word(*citer)]);
    
    queue.push_swap(context_count);
  }
  
  queue.push(std::make_pair(context_type(), count_type(0)));
  thread->join();
}

template <typename Stream>
void index_ngram_reducer(intercomm_type& mapper, ngram_type& ngram, Stream& os_count)
{
  typedef VocabMap vocab_map_type;

  const int mpi_rank = MPI::COMM_WORLD.Get_rank();
  const int mpi_size = MPI::COMM_WORLD.Get_size();
  
  vocab_map_type vocab_map(ngram.index.vocab());
  
  for (int order = 2; /**/; ++ order) {
    
    int count_file_size = 0;
    if (mpi_rank == 0)
      mapper.comm.Recv(&count_file_size, 1, MPI::INT, 0, size_tag);
    MPI::COMM_WORLD.Bcast(&count_file_size, 1, MPI::INT, 0);
    
    if (count_file_size == 0) break;
    
    if (debug)
      std::cerr << "reducer: rank: " << mpi_rank << " order: " << order << std::endl;
    
    index_ngram_reducer(mapper, ngram, os_count, vocab_map);
  }
}

int getoptions(int argc, char** argv)
{
  const int mpi_rank = MPI::COMM_WORLD.Get_rank();
  const int mpi_size = MPI::COMM_WORLD.Get_size();

  namespace po = boost::program_options;
  
  po::options_description desc("options");
  desc.add_options()
    ("ngram",     po::value<path_type>(&ngram_file)->default_value(ngram_file),   "ngram counts in Google format")
    ("output",    po::value<path_type>(&output_file)->default_value(output_file), "output in binary format")
    ("temporary", po::value<path_type>(&temporary_dir),                           "temporary directory")
    
    ("prog",       po::value<path_type>(&prog_name),  "this binary")
    ("host",       po::value<std::string>(&host),     "host name")
    ("hostfile",   po::value<std::string>(&hostfile), "hostfile name")    
    
    ("debug", po::value<int>(&debug)->implicit_value(1), "debug level")
    ("help", "help message");
  
  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc, po::command_line_style::unix_style & (~po::command_line_style::allow_guessing)), vm);
  po::notify(vm);
  
  if (vm.count("help")) {
    if (mpi_rank == 0)
      std::cout << argv[0] << " [options]" << '\n' << desc << '\n';
    return 1;
  }
  
  return 0;
}

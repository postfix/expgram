//
//  Copyright(C) 2009-2013 Taro Watanabe <taro.watanabe@nict.go.jp>
//

#define BOOST_SPIRIT_THREADSAFE

#include <boost/spirit/include/qi.hpp>
#include <boost/spirit/include/karma.hpp>

#include <iostream>
#include <iterator>

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#include <utils/compress_stream.hpp>
#include <utils/program_options.hpp>
#include <utils/mathop.hpp>
#include <utils/resource.hpp>
#include <utils/hashmurmur3.hpp>
#include <utils/array_power2.hpp>

#include <expgram/NGram.hpp>
#include <expgram/Sentence.hpp>
#include <expgram/Vocab.hpp>

typedef boost::filesystem::path path_type;

typedef expgram::NGram    ngram_type;
typedef expgram::Word     word_type;
typedef expgram::Vocab    vocab_type;
typedef expgram::Sentence sentence_type;

typedef ngram_type::state_type state_type;

struct NGramCache : public utils::hashmurmur3<size_t>
{

  struct Cache
  {
    state_type         curr_;
    state_type         next_;
    word_type::id_type word_;
    float              prob_;
    
    Cache() : curr_(), next_(), word_(word_type::id_type(-1)), prob_(std::numeric_limits<float>::quiet_NaN()) {}
  };
  
  typedef Cache cache_type;
  typedef utils::array_power2<cache_type, 1024 * 1024, std::allocator<cache_type> > cache_set_type;
  
  typedef utils::hashmurmur3<size_t> hasher_type;
  
  NGramCache(const ngram_type& ngram) : caches_(), ngram_(ngram) {}
  
  const std::pair<state_type, float> operator()(const state_type& state, const word_type::id_type& id) const
  {
    const size_t cache_pos = hasher_type()(state, id) & (caches_.size() - 1);

    cache_type& cache = const_cast<cache_type&>(caches_[cache_pos]);
    
    if (cache.curr_ != state || cache.word_ != id || cache.prob_ == std::numeric_limits<float>::quiet_NaN()) {
      cache.curr_ = state;
      cache.word_ = id;
      
      const std::pair<state_type, float> result = ngram_.logprob(state, id);

      cache.next_ = result.first;
      cache.prob_ = result.second;
    }
    
    return std::make_pair(cache.next_, cache.prob_);
  }
  
  cache_set_type caches_;
  const ngram_type& ngram_;
};

path_type ngram_file;
path_type input_file = "-";
path_type output_file = "-";

int order = 0;

int shards = 4;
int verbose = 0;
int debug = 0;

int getoptions(int argc, char** argv);

int main(int argc, char** argv)
{
  try {
    if (getoptions(argc, argv) != 0) 
      return 1;

    namespace karma = boost::spirit::karma;
    namespace standard = boost::spirit::standard;
        
    ngram_type ngram(ngram_file, shards, debug);
    NGramCache cache(ngram);
    
    sentence_type sentence;
    
    const bool flush_output = (output_file == "-" || (boost::filesystem::exists(output_file) && ! boost::filesystem::is_regular_file(output_file)));
    
    utils::compress_istream is(input_file);
    utils::compress_ostream os(output_file, 1024 * 1024 * (! flush_output));
    
    order = (order <= 0 ? ngram.index.order() : order);
    
    const word_type::id_type bos_id = ngram.index.vocab()[vocab_type::BOS];
    const word_type::id_type eos_id = ngram.index.vocab()[vocab_type::EOS];
    const word_type::id_type unk_id = ngram.index.vocab()[vocab_type::UNK];
    const word_type::id_type none_id = word_type::id_type(-1);
    
    const state_type state_bos = ngram.index.next(state_type(), bos_id);
    
    size_t num_word = 0;
    size_t num_sentence = 0;

    utils::resource start;
    
    while (is >> sentence) {
      // add BOS and EOS
      
      if (sentence.empty()) continue;
      
      int oov = 0;
      double logprob = 0.0;
      state_type state = state_bos;
      sentence_type::const_iterator siter_end = sentence.end();
      for (sentence_type::const_iterator siter = sentence.begin(); siter != siter_end; ++ siter) {
	const word_type::id_type id = ngram.index.vocab()[*siter];
	
	//const std::pair<state_type, float> result = ngram.logprob(state, id);
	const std::pair<state_type, float> result = cache(state, id);
	
	if (verbose)
	  if (! karma::generate(std::ostream_iterator<char>(os),
				standard::string << '=' << karma::uint_ << ' ' << karma::int_ << ' ' << karma::double_ << '\n',
				*siter, id, ngram.index.order(result.first), result.second))
	    throw std::runtime_error("generation failed");

	oov += (id == unk_id) || (id == none_id);
	
	state = result.first;
	logprob += result.second;
      }
      
      //const std::pair<state_type, float> result = ngram.logprob(state, eos_id);
      const std::pair<state_type, float> result = cache(state, eos_id);
      
      if (verbose)
	if (! karma::generate(std::ostream_iterator<char>(os),
			      standard::string << '=' << karma::uint_ << ' ' << karma::int_ << ' ' << karma::double_ << '\n',
			      vocab_type::EOS, eos_id, ngram.index.order(result.first), result.second))
	  throw std::runtime_error("generation failed");
      
      logprob += result.second;
      
      if (! karma::generate(std::ostream_iterator<char>(os),
			    karma::double_ << ' ' << karma::int_ << '\n',
			    logprob, oov))
	throw std::runtime_error("generation failed");

      ++ num_sentence;
      num_word += sentence.size();
    }
    
    utils::resource end;
    
    if (debug)
      std::cerr << "queries: " << (num_word + num_sentence) << std::endl
		<< "cpu:    " << 1e-3 * (num_word + num_sentence) / (end.cpu_time() - start.cpu_time()) << " queries/ms" << std::endl
		<< "user:   " << 1e-3 * (num_word + num_sentence) / (end.user_time() - start.user_time()) << " queries/ms" << std::endl
		<< "thread: " << 1e-3 * (num_word + num_sentence) / (end.thread_time() - start.thread_time()) << " queries/ms" << std::endl;
  }
  catch (std::exception& err) {
    std::cerr << "error: " << err.what() << std::endl;
    return 1;
  }
  return 0;
}

int getoptions(int argc, char** argv)
{
  namespace po = boost::program_options;
  
  po::options_description desc("options");
  desc.add_options()
    ("ngram",  po::value<path_type>(&ngram_file)->default_value(ngram_file),   "ngram in ARPA or expgram format")
    ("input",  po::value<path_type>(&input_file)->default_value(input_file),   "input")
    ("output", po::value<path_type>(&output_file)->default_value(output_file), "output")
    
    ("order",       po::value<int>(&order)->default_value(order),              "ngram order")
    
    ("shard",   po::value<int>(&shards)->default_value(shards),                 "# of shards (or # of threads)")
    ("verbose", po::value<int>(&verbose)->implicit_value(1), "verbose level")
    ("debug",   po::value<int>(&debug)->implicit_value(1),   "debug level")
    ("help", "help message");
  
  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc, po::command_line_style::unix_style & (~po::command_line_style::allow_guessing)), vm);
  po::notify(vm);
  
  if (vm.count("help")) {
    std::cout << argv[0] << " [options]" << '\n' << desc << '\n';
    return 1;
  }
  
  return 0;
}

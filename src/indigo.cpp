/*
============================================================================
Indigo: InDel Discovery in Sanger Chromatograms
============================================================================
Copyright (C) 2017 Tobias Rausch

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
============================================================================
Contact: Tobias Rausch (rausch@embl.de)
============================================================================
*/

#define BOOST_DISABLE_ASSERTS
#include <boost/multi_array.hpp>

#include "abif.h"
#include "refslice.h"
#include "align.h"
#include "gotoh.h"

using namespace indigo;
using namespace sdsl;

struct Config {
  bool fullabif;
  uint16_t filetype;   //0: *fa.gz, 1: *.fa, 2: *.ab1
  uint16_t kmer;
  uint16_t maxindel;
  uint16_t trim;
  uint16_t linelimit;
  uint16_t madc;
  float pratio;
  std::string outprefix;
  boost::filesystem::path ab;
  boost::filesystem::path genome;
};

int main(int argc, char** argv) {
  Config c;
  
  // Parameter
  boost::program_options::options_description generic("Generic options");
  generic.add_options()
    ("help,?", "show help message")
    ("genome,g", boost::program_options::value<boost::filesystem::path>(&c.genome), "(gzipped) fasta or wildtype ab1 file")
    ("pratio,p", boost::program_options::value<float>(&c.pratio)->default_value(0.33), "peak ratio to call base")
    ("kmer,k", boost::program_options::value<uint16_t>(&c.kmer)->default_value(15), "kmer size")
    ("trim,t", boost::program_options::value<uint16_t>(&c.trim)->default_value(50), "trim size for Sanger trace")
    ("maxindel,m", boost::program_options::value<uint16_t>(&c.maxindel)->default_value(1000), "max. indel size in Sanger trace")
    ;

  boost::program_options::options_description otp("Output options");
  otp.add_options()
    ("output,o", boost::program_options::value<std::string>(&c.outprefix)->default_value("align"), "output file prefix")
    ("linelimit,l", boost::program_options::value<uint16_t>(&c.linelimit)->default_value(60), "alignment line length")
    ("fullabif,f", "full chromatogram output")
    ;

  boost::program_options::options_description hidden("Hidden options");
  hidden.add_options()
    ("madc,c", boost::program_options::value<uint16_t>(&c.madc)->default_value(5), "MAD cutoff")
    ("input-file", boost::program_options::value<boost::filesystem::path>(&c.ab), "ab1")
    ;

  boost::program_options::positional_options_description pos_args;
  pos_args.add("input-file", -1);

  boost::program_options::options_description cmdline_options;
  cmdline_options.add(generic).add(otp).add(hidden);
  boost::program_options::options_description visible_options;
  visible_options.add(generic).add(otp);
  boost::program_options::variables_map vm;
  boost::program_options::store(boost::program_options::command_line_parser(argc, argv).options(cmdline_options).positional(pos_args).run(), vm);
  boost::program_options::notify(vm);

  // Check command line arguments
  if ((vm.count("help")) || (!vm.count("input-file"))) {
    std::cout << "Usage: " << argv[0] << " [OPTIONS] trace.ab1" << std::endl;
    std::cout << visible_options << "\n";
    return -1;
  }
  if (c.maxindel < 1) c.maxindel = 1;

  // Extended ABIF output?
  if (vm.count("fullabif")) c.fullabif = true;
  else c.fullabif = false;
  
  // Show cmd
  boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
  std::cout << '[' << boost::posix_time::to_simple_string(now) << "] ";
  for(int i=0; i<argc; ++i) { std::cout << argv[i] << ' '; }
  std::cout << std::endl;

  // Load *.ab1 file
  now = boost::posix_time::second_clock::local_time();
  std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Load ab1 file" << std::endl;
  
  // Read *.ab1 file
  Trace tr;
  if (!readab(c.ab, tr)) return -1;

  // Call bases
  BaseCalls bc;
  basecall(tr, bc, c.pratio);

  // Write ABIF signal
  boost::filesystem::path outabif(c.outprefix + ".abif");
  std::ofstream ofile(outabif.string().c_str());
  if (!c.fullabif) {
    ofile << "basenum\tpeakA\tpeakC\tpeakG\tpeakT\tprimary\tsecondary\tconsensus" << std::endl;
    for(uint32_t i = 0; i<bc.primary.size(); ++i) {
      ofile << i << "\t";
      for(uint32_t k =0; k<4; ++k) {
	ofile << bc.peak[k][i] << "\t";
      }
      ofile << bc.primary[i] << "\t" << bc.secondary[i] << "\t" << bc.consensus[i] << std::endl;
    }
  } else {
    ofile << "pos\tpeakA\tpeakC\tpeakG\tpeakT" << std::endl;
    for(uint32_t i = 0; i<tr.traceACGT[0].size(); ++i) {
      ofile << i << "\t";
      for(uint32_t k =0; k<4; ++k) {
	ofile << tr.traceACGT[k][i] << "\t";
      }
      ofile << std::endl;
    }
  }
  ofile.close();

  // Align against genome
  if (vm.count("genome")) {

    // Reference index
    csa_wt<> fm_index;
    ReferenceSlice rs;
    
    // What kind of reference?
    std::ifstream ifile(c.genome.string().c_str(), std::ios::binary | std::ios::in);
    if (ifile.is_open()) {
      char fcode[4];
      ifile.seekg(0);
      ifile.read(fcode, 4);
      if (((uint8_t)fcode[0] == (uint8_t)0x1f) && ((uint8_t)fcode[1] == (uint8_t)0x8b)) {
	// Gzipped fasta
	c.filetype = 0;
	boost::filesystem::path op = c.genome.parent_path() / c.genome.stem();
	boost::filesystem::path outfile(op.string() + ".dump");
	std::string index_file = op.string() + ".fm9";

	// Load FM index
	now = boost::posix_time::second_clock::local_time();
	std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Load FM-Index" << std::endl;
	if (!load_from_file(fm_index, index_file)) {
	  std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Build FM-Index" << std::endl;
	  // Dump fasta
	  bool firstSeq = true;
	  std::ofstream tmpout(outfile.string().c_str());
	  std::ifstream file(c.genome.string().c_str(), std::ios_base::in | std::ios_base::binary);
	  boost::iostreams::filtering_streambuf<boost::iostreams::input> dataIn;
	  dataIn.push(boost::iostreams::gzip_decompressor());
	  dataIn.push(file);
	  std::istream instream(&dataIn);
	  std::string line;
	  while(std::getline(instream, line)) {
	    if (line.find(">") == 0) {
	      if (!firstSeq) tmpout << std::endl;
	      else firstSeq = false;
	    } else {
	      tmpout << boost::to_upper_copy(line);
	    }
	  }
	  tmpout << std::endl;
	  file.close();
	  tmpout.close();
	  
	  now = boost::posix_time::second_clock::local_time();
	  std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Create FM-Index" << std::endl;
      
	  // Build index
	  construct(fm_index, outfile.string().c_str(), 1);
	  store_to_file(fm_index, index_file);
	  boost::filesystem::remove(outfile);
	}
      } else if (std::string(fcode) == "ABIF") {
	// ab1 wildtype file
	c.filetype = 2;

	now = boost::posix_time::second_clock::local_time();
	std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Load ab1 wildtype" << std::endl;
	Trace wt;
	if (!readab(c.genome, wt)) return -1;
	BaseCalls wtbc;
	basecall(wt, wtbc, c.pratio);
	construct_im(fm_index, wtbc.consensus.c_str(), 1);
	rs.chr = "wildtype";
	rs.refslice = wtbc.consensus;
      }
      else if (fcode[0] == '>') {
	// Single FASTA file
	c.filetype = 1;

	// Read FASTA
	rs.chr = "";
	rs.refslice = "";
	now = boost::posix_time::second_clock::local_time();
	std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Load FASTA reference" << std::endl;
	std::ifstream fafile(c.genome.string().c_str());
	if (fafile.good()) {
	  std::string line;
	  while(std::getline(fafile, line)) {
	    if (!line.empty()) {
	      if (line[0] == '>') {
		if (!rs.chr.empty()) {
		  std::cerr << "Only single-chromosome FASTA files are supported. If you have a multi-FASTA file please use bgzip and index the FASTA file with samtools faidx!" << std::endl;
		  return -1;
		}
		rs.chr = line.substr(1);
	      } else {
		rs.refslice += boost::to_upper_copy(line);
	      }
	    }
	  }
	  fafile.close();
	}
	// Check FASTA
	for(uint32_t k = 0; k<rs.refslice.size(); ++k) {
	  if ((rs.refslice[k] != 'A') && (rs.refslice[k] != 'C') && (rs.refslice[k] != 'G') && (rs.refslice[k] != 'T') && (rs.refslice[k] != 'N')) {
	    std::cerr << "FASTA file contains nucleotides != [ACGTN]." << std::endl;
	    return -1;
	  }
	}
	construct_im(fm_index, rs.refslice.c_str(), 1);
      } else {
	std::cerr << "Couldn't recognize reference file format!" << std::endl;
	return -1;
      }
    }
    ifile.close();


    now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Find Reference Match" << std::endl;
    
    // Identify position of indel shift in Sanger trace
    if (!findBreakpoint(c, bc)) return -1;
    
    // Get reference slice
    if (!getReferenceSlice(c, fm_index, bc, rs)) return -1;

    // Find breakpoint for hom. indels
    if (!bc.indelshift) {
      now = boost::posix_time::second_clock::local_time();
      std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Find Alignment Break" << std::endl;
      if (!findHomozygousBreakpoint(c, bc, rs)) return -1;
      if (!getReferenceSlice(c, fm_index, bc, rs)) return -1;
    }
    
    now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Decompose Chromatogram" << std::endl;
    
    // Decompose alleles
    if (!decomposeAlleles(c, bc, rs)) return -1;

    now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Align to reference" << std::endl;
    
    // Plot alignments
    typedef boost::multi_array<char, 2> TAlign;
    TAlign alignPrimary;
    AlignConfig<true, false> semiglobal;
    DnaScore<int> sc(5, -4, -50, 0);
    std::string pri = bc.primary.substr(c.trim, bc.primary.size() - (2*c.trim));
    gotoh(pri, rs.refslice, alignPrimary, semiglobal, sc);
    plotAlignment(c, alignPrimary, rs, 1);
    
    typedef boost::multi_array<char, 2> TAlign;
    TAlign alignSecondary;
    std::string sec = bc.secondary.substr(c.trim, bc.secondary.size() - (2*c.trim));
    gotoh(sec, rs.refslice, alignSecondary, semiglobal, sc);
    plotAlignment(c, alignSecondary, rs, 2);
  }
    
  now = boost::posix_time::second_clock::local_time();
  std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Done." << std::endl;

  return 0;
}
#include <functional>
#include <exception>
#include <iostream>
#include <cstdlib>
#include <memory>
#include <vector>
#include <string>

#include <aff3ct.hpp>
using namespace aff3ct;

#include "common/Block.hpp"
#include "common/Splitter/Splitter.hpp"

struct params
{
	const size_t bl_buffer_size = 16;     // the circular buffer size in the pipeline blocks
	const size_t bl_n_threads   =  1;     // the number of threads in one pipeline block
	const float ebn0_min        =  0.00f; // minimum SNR value
	const float ebn0_max        = 10.01f; // maximum SNR value
	const float ebn0_step       =  1.00f; // SNR step
	      float R;                        // code rate (R=K/N)

	std::unique_ptr<factory::Source          > source;
	std::unique_ptr<factory::Codec_repetition> codec;
	std::unique_ptr<factory::Modem           > modem;
	std::unique_ptr<factory::Channel         > channel;
	std::unique_ptr<factory::Monitor_BFER    > monitor;
	std::unique_ptr<factory::Terminal        > terminal;
};
void init_params(int argc, char** argv, params &p);

struct modules
{
	std::unique_ptr<module::Source<>>       source;
	std::unique_ptr<module::Splitter<>>     splitter;
	std::unique_ptr<module::Codec_SIHO<>>   codec;
	std::unique_ptr<module::Modem<>>        modulator;
	std::unique_ptr<module::Modem<>>        demodulator;
	std::unique_ptr<module::Channel<>>      channel;
	std::unique_ptr<module::Monitor_BFER<>> monitor;
	                module::Encoder<>*      encoder;
	                module::Decoder_SIHO<>* decoder;
	std::vector<const module::Module*>      list; // list of module pointers declared in this structure
};
void init_modules(const params &p, modules &m);

struct utils
{
	std::unique_ptr<tools::Sigma<>>               noise;     // a sigma noise type
	std::vector<std::unique_ptr<tools::Reporter>> reporters; // list of reporters dispayed in the terminal
	std::unique_ptr<tools::Terminal>              terminal;  // manage the output text in the terminal
};
void init_utils(const params &p, const modules &m, utils &u);

int main(int argc, char** argv)
{
	// get the AFF3CT version
	const std::string v = "v" + std::to_string(tools::version_major()) + "." +
	                            std::to_string(tools::version_minor()) + "." +
	                            std::to_string(tools::version_release());

	std::cout << "#----------------------------------------------------------"      << std::endl;
	std::cout << "# This is a basic program using the AFF3CT library (" << v << ")" << std::endl;
	std::cout << "# Feel free to improve it as you want to fit your needs."         << std::endl;
	std::cout << "#----------------------------------------------------------"      << std::endl;
	std::cout << "#"                                                                << std::endl;

	params  p; init_params (argc, argv, p); // create and initialize the parameters from the command line with factories
	modules m; init_modules(p, m         ); // create and initialize the modules
	utils   u; init_utils  (p, m, u      ); // create and initialize the utils

	// display the legend in the terminal
	u.terminal->legend();

	using namespace module;
	Block bl_source     ((*m.source     )[src::tsk::generate    ], p.bl_buffer_size, p.bl_n_threads);
	Block bl_encoder    ((*m.encoder    )[enc::tsk::encode      ], p.bl_buffer_size, p.bl_n_threads);
	Block bl_modulator  ((*m.modulator  )[mdm::tsk::modulate    ], p.bl_buffer_size, p.bl_n_threads);
	Block bl_channel    ((*m.channel    )[chn::tsk::add_noise   ], p.bl_buffer_size, p.bl_n_threads);
	Block bl_demodulator((*m.demodulator)[mdm::tsk::demodulate  ], p.bl_buffer_size, p.bl_n_threads);
	Block bl_decoder    ((*m.decoder    )[dec::tsk::decode_siho ], p.bl_buffer_size, p.bl_n_threads);
	Block bl_splitter   ((*m.splitter   )[spl::tsk::split       ], p.bl_buffer_size, p.bl_n_threads);
	Block bl_monitor    ((*m.monitor    )[mnt::tsk::check_errors], p.bl_buffer_size, p.bl_n_threads);

	std::vector<Block*> blocks = { &bl_source,      &bl_encoder, &bl_modulator, &bl_channel,
	                               &bl_demodulator, &bl_decoder, &bl_splitter,  &bl_monitor };

	// sockets binding (connect the sockets of the tasks = fill the input sockets with the output sockets)
	bl_splitter   .bind("U_K" , bl_source     , "U_K" );
	bl_encoder    .bind("U_K" , bl_splitter   , "V_K1");
	bl_modulator  .bind("X_N1", bl_encoder    , "X_N" );
	bl_channel    .bind("X_N" , bl_modulator  , "X_N2");
	bl_demodulator.bind("Y_N1", bl_channel    , "Y_N" );
	bl_decoder    .bind("Y_N" , bl_demodulator, "Y_N2");
	bl_monitor    .bind("U"   , bl_splitter   , "V_K2");
	bl_monitor    .bind("V"   , bl_decoder    , "V_K" );

	// loop over the various SNRs
	for (auto ebn0 = p.ebn0_min; ebn0 < p.ebn0_max; ebn0 += p.ebn0_step)
	{
		// compute the current sigma for the channel noise
		const auto esn0  = tools::ebn0_to_esn0 (ebn0, p.R, p.modem->bps);
		const auto sigma = tools::esn0_to_sigma(esn0, p.modem->cpm_upf );

		u.noise->set_noise(sigma, ebn0, esn0);

		// update the sigma of the modem and the channel
		m.codec      ->set_noise(*u.noise);
		m.demodulator->set_noise(*u.noise);
		m.channel    ->set_noise(*u.noise);

		// display the performance (BER and FER) in real time (in a separate thread)
		u.terminal->start_temp_report();

		bool is_done = false;
		std::thread th_done_verif([&is_done, &m, &u]()
		{
			while (!m.monitor->fe_limit_achieved() && !u.terminal->is_interrupt());
			is_done = true;
		});

		// run the simulation chain
		for (auto &b : blocks) b->run  (is_done);
		for (auto &b : blocks) b->join (       );
		for (auto &b : blocks) b->reset(       );

		th_done_verif.join();

		// display the performance (BER and FER) in the terminal
		u.terminal->final_report();

		// reset the monitor and the terminal for the next SNR
		m.monitor->reset();
		u.terminal->reset();

		// if user pressed Ctrl+c twice, exit the SNRs loop
		if (u.terminal->is_over()) break;
	}

	// display the statistics of the tasks (if enabled)
	std::cout << "#" << std::endl;
	std::vector<std::vector<const Task*>> bl_tasks = { bl_source     .get_tasks(), bl_encoder.get_tasks(),
	                                                   bl_modulator  .get_tasks(), bl_channel.get_tasks(),
	                                                   bl_demodulator.get_tasks(), bl_decoder.get_tasks(),
	                                                   bl_splitter   .get_tasks(), bl_monitor.get_tasks() };
	tools::Stats::show(bl_tasks, true);
	std::cout << "# End of the simulation" << std::endl;

	return 0;
}

void init_params(int argc, char** argv, params &p)
{
	p.source   = std::unique_ptr<factory::Source          >(new factory::Source          ());
	p.codec    = std::unique_ptr<factory::Codec_repetition>(new factory::Codec_repetition());
	p.modem    = std::unique_ptr<factory::Modem           >(new factory::Modem           ());
	p.channel  = std::unique_ptr<factory::Channel         >(new factory::Channel         ());
	p.monitor  = std::unique_ptr<factory::Monitor_BFER    >(new factory::Monitor_BFER    ());
	p.terminal = std::unique_ptr<factory::Terminal        >(new factory::Terminal        ());

	std::vector<factory::Factory*> params_list = { p.source .get(), p.codec  .get(), p.modem   .get(),
	                                               p.channel.get(), p.monitor.get(), p.terminal.get() };

	// parse the command for the given parameters and fill them
	tools::Command_parser cp(argc, argv, params_list, true);
	if (cp.parsing_failed())
	{
		cp.print_help    ();
		cp.print_warnings();
		cp.print_errors  ();
		std::exit(1);
	}

	std::cout << "# Simulation parameters: " << std::endl;
	tools::Header::print_parameters(params_list); // display the headers (= print the AFF3CT parameters on the screen)
	std::cout << "#" << std::endl;
	cp.print_warnings();

	p.R = (float)p.codec->enc->K / (float)p.codec->enc->N_cw; // compute the code rate
}

void init_modules(const params &p, modules &m)
{
	m.source      = std::unique_ptr<module::Source      <>>(p.source ->build()                 );
	m.splitter    = std::unique_ptr<module::Splitter    <>>(new module::Splitter<>(p.source->K));
	m.codec       = std::unique_ptr<module::Codec_SIHO  <>>(p.codec  ->build()                 );
	m.modulator   = std::unique_ptr<module::Modem       <>>(p.modem  ->build()                 );
	m.demodulator = std::unique_ptr<module::Modem       <>>(p.modem  ->build()                 );
	m.channel     = std::unique_ptr<module::Channel     <>>(p.channel->build()                 );
	m.monitor     = std::unique_ptr<module::Monitor_BFER<>>(p.monitor->build()                 );
	m.encoder     = m.codec->get_encoder().get();
	m.decoder     = m.codec->get_decoder_siho().get();

	m.list = { m.source .get(), m.splitter.get(), m.modulator.get(), m.demodulator.get(),
	           m.channel.get(), m.monitor .get(), m.encoder,         m.decoder };

	m.modulator  ->set_custom_name("Modulator"  );
	m.demodulator->set_custom_name("Demodulator");

	// configuration of the module tasks
	for (auto& mod : m.list)
		for (auto& tsk : mod->tasks)
		{
			tsk->set_autoalloc  (true ); // enable the automatic allocation of the data in the tasks
			tsk->set_autoexec   (false); // disable the auto execution mode of the tasks
			tsk->set_debug      (false); // disable the debug mode
			tsk->set_debug_limit(16   ); // display only the 16 first bits if the debug mode is enabled
			tsk->set_stats      (true ); // enable the statistics

			// enable the fast mode (= disable the useless verifs in the tasks) if there is no debug and stats modes
			if (!tsk->is_debug() && !tsk->is_stats())
				tsk->set_fast(true);
		}

	// reset the memory of the decoder after the end of each communication
	// TODO: this is not done when using pipeline Block, be careful!!!
	m.monitor->add_handler_check(std::bind(&module::Decoder::reset, m.decoder));
}

void init_utils(const params &p, const modules &m, utils &u)
{
	// create a sigma noise type
	u.noise = std::unique_ptr<tools::Sigma<>>(new tools::Sigma<>());
	// report the noise values (Es/N0 and Eb/N0)
	u.reporters.push_back(std::unique_ptr<tools::Reporter>(new tools::Reporter_noise<>(*u.noise)));
	// report the bit/frame error rates
	u.reporters.push_back(std::unique_ptr<tools::Reporter>(new tools::Reporter_BFER<>(*m.monitor)));
	// report the simulation throughputs
	u.reporters.push_back(std::unique_ptr<tools::Reporter>(new tools::Reporter_throughput<>(*m.monitor)));
	// create a terminal that will display the collected data from the reporters
	u.terminal = std::unique_ptr<tools::Terminal>(p.terminal->build(u.reporters));
}
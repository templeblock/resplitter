/*
 * Copyright (C) 2011 Todd A Fisher <todd.fisher@gmail.com> and Ramon Martinez <rampa@encomix.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Given an input wav file locate quiet portions of audio stream and clip them, 
 * outputing a new stream of audio chunks that include only the noisy parts of the audio file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sndfile.h>
#include "splitter.h"

#define usage() fprintf(stderr, "usage: resplitter -i infile.wav  -t 300 -d 0.1 -o outfile.wav\n" \
"\t-b: how large of a buffer will impact how many frames we look at for each root means. larger byte buffer more frames are compressed into a single root means square\n" \
"\t-i: the input wave audio file to split up by silence break points\n" \
"\t-o: the output wave audio file with silence stripped if not specified, the same as input\n" \
"\t-v 1: be Verbose\n" \
"\t-t: a wave magnitude threshold e.g. anything greater than this value will be considered sound\n" \
"\t-d: if a chunk is created and it is shorter than this length in time it will be discarded as noise\n" \
"\t-D: if a chunk is created and is greater in length than it will be reprocessed using an increased threshold(t) and decreased byte buffer(b).\n" \
"\t-w: can be nice to see the wave curve useful when trying to understand the audio file... cat out.audiowave | ruby plot.viz.rb && open out.wave.png\n" \
);

int totalChunks=0;
const char *input_wav_file = NULL;
const char *output_wav_file = NULL;
int verbose=0;

static SilenceDetector* silence_detector_parse_cli(int argc, char **argv) {
  int ch;
  double max_duration = 10; // seconds
  double min_duration = 1; // seconds
  int threshold = 30;
  int buffer_size = 1024;
  const char *output_wave = NULL;

  opterr = 0;

  if (argc < 2) {
    usage();
    exit(1);
  }

  while ((ch = getopt(argc, argv, "b:i:o:t:d:D:w:v:")) != -1) {
    switch (ch) {
    case 'b':
      buffer_size = atoi(optarg);
      break;
    case 'i':
      input_wav_file = optarg;
      break;
    case 'o':
      output_wav_file = optarg;
      break;
    case 'v':
      verbose=1;
      break;
    case 't':
      threshold = atoi(optarg);
      break;
    case 'd':
      min_duration = atof(optarg);
      break;
    case 'D':
      max_duration = atof(optarg);
      break;
    case 'w':
      output_wave = optarg;
      break;
    case '?':
    default:
      usage();
      exit(0);
    }
  }
  argc -= optind;
  argv += optind;

  if (argc != 0) {
    usage();
    exit(1);
  }

  if (!output_wav_file) 
   output_wav_file = input_wav_file;
   
  if (verbose ==1) 
   printf("chunk constraints: [%.2f, %2.f] seconds\nsilence threshold: %d\noutput wave: %s\n", min_duration, max_duration, threshold, output_wave);

  return silence_detector_new(input_wav_file, buffer_size, threshold, min_duration, max_duration, output_wave);
}

int main(int argc, char **argv) {

  SilenceDetector *detector = silence_detector_parse_cli(argc, argv);

  silence_detector_split_audio(detector);

  silence_detector_free(detector);

  merge(totalChunks,input_wav_file,output_wav_file);
  
  return 0;
}

static SilenceDetector* silence_detector_new(const char *file, size_t buffer_size, int threshold, double min_duration, double max_duration, const char *output_wave) {
  SilenceDetector *ptr = (SilenceDetector*)malloc(sizeof(SilenceDetector));
  memset(ptr, 0, sizeof(SilenceDetector));


	ptr->sndfile = sf_open(file, SFM_READ, &(ptr->sndfile_info));

	if (!ptr->sndfile) {
		fprintf(stderr, "Failed to open: %s with error: %s\n", file, sf_strerror(NULL));
    free(ptr);
		return NULL;
	}

  ptr->file_name = file;
  ptr->threshold = threshold;
  ptr->min_duration = min_duration;
  ptr->max_duration = max_duration;
  ptr->output_wave = output_wave;

  /*
  printf("file: %s\n\tframes:\t\t%llu\n\tsamplerate:\t%d\n\tchannels:\t%d\n\tformat:\t\t%d\n\tsections:\t%d\n\tseekable:\t%d\n", file, ptr->sndfile_info.frames,
                                                                                                         ptr->sndfile_info.samplerate,
                                                                                                         ptr->sndfile_info.channels,
                                                                                                         ptr->sndfile_info.format,
                                                                                                         ptr->sndfile_info.sections,
                                                                                                         ptr->sndfile_info.seekable);
  printf("duration: %.2f seconds\n", (double)ptr->sndfile_info.frames / (double)ptr->sndfile_info.samplerate);
  // itunes for example will round seconds up... should we do this too?
  printf("duration: %.2f seconds\n", ceil((double)ptr->sndfile_info.frames / (double)ptr->sndfile_info.samplerate));
  */
	ptr->buffer_size = buffer_size; // read 16 frames at a time?
	ptr->buffer = (int*)malloc((ptr->buffer_size*2) * sizeof(int)); // 2x size we are not sure how the frame is being read/written in libsndfile...

  return ptr;
}

static void silence_detector_free(SilenceDetector *ptr) {
  if (ptr) {
    sf_close(ptr->sndfile);
    free(ptr->buffer);

    if (ptr->out_sndfile) {
      sf_close(ptr->out_sndfile);
    }

    if (ptr->last_chunk_file_name) {
      free(ptr->last_chunk_file_name);
    }

    free(ptr);
  }
}

static int silence_detector_split_audio(SilenceDetector *ptr) {
  sf_count_t offset = 0;
  sf_count_t prev_offset = 0;
  sf_count_t read_frames = 0;
  sf_count_t read_total = 0;
  FILE *owave = NULL;

  if (ptr->output_wave) {
    owave = fopen(ptr->output_wave, "wb");
    if (!owave) {
      fprintf(stderr, "Error opening output wave file: %s\n", ptr->output_wave);
      exit(4);
    }
  }

  silence_detector_create_audio_chunk(ptr);

  do {
    prev_offset = offset;
    read_frames = silence_detector_locate_silence_offset(ptr, offset, &offset, owave);
    read_total += read_frames;
    //printf("p: %llu, o %llu\n", prev_offset, offset);
    //if (prev_offset == offset) {
    //  printf("silence from: %llu to %llu\n", offset, read_total);
    //}
  } while (read_frames > 0);

	//printf("\n");
  if (owave) {
    fclose(owave);
  }

  return 0;
}

static void silence_detector_create_audio_chunk(SilenceDetector *ptr) {
  char *out_chunk_file_name = silence_detector_create_chunk_file_name(ptr);
	ptr->out_sndfile = sf_open(out_chunk_file_name, SFM_WRITE, &(ptr->sndfile_info));
  if (ptr->last_chunk_file_name) {
    free(ptr->last_chunk_file_name);
  }
  ptr->last_chunk_file_name = out_chunk_file_name;

  if (!ptr->out_sndfile) {
    fprintf(stderr, "Failed to create audio chunk!\n");
    exit(3);
  }
}

static char* silence_detector_create_chunk_file_name(SilenceDetector *ptr) {
  size_t file_name_length = (strlen(ptr->file_name)+1 + strlen("chunk-.wav") + 1 + ptr->chunk_count + 1);
  char *out_chunk_file_name = (char*)malloc(file_name_length* sizeof(char));
  memset(out_chunk_file_name, 0, file_name_length);
  snprintf(out_chunk_file_name, file_name_length, "%s.chunk%d.wav", ptr->file_name, ptr->chunk_count++);
  totalChunks=ptr->chunk_count;
  return out_chunk_file_name;
}

static double read_sound_file_duration(const char *filepath) {
	SNDFILE *sndfile;
	SF_INFO sndfile_info;
  double duration;
  memset(&sndfile_info, 0, sizeof(SF_INFO));

	sndfile = sf_open(filepath, SFM_READ, &sndfile_info);
  if (sndfile) {
    duration = ((double)sndfile_info.frames) / ((double)sndfile_info.samplerate);
    
    if (verbose ==1) 
       fprintf(stderr, "duration: %.2f, frames: %.2f, rate: %.2f\n", duration, (double)sndfile_info.frames, (double)sndfile_info.samplerate);
       
    sf_close(sndfile);
    return duration;
  }

  return 0;
}

static sf_count_t silence_detector_locate_silence_offset(SilenceDetector *ptr, sf_count_t soff, sf_count_t *offset, FILE *owave) {
	int i, wave_length, avg = 0;
  double chunk_duration, rms = 0.0;
	sf_count_t read_frames;

  memset(ptr->buffer, 0, ptr->buffer_size * sizeof(int));
	read_frames = sf_readf_int(ptr->sndfile, ptr->buffer, ptr->buffer_size);

	for ( i = 0; i < read_frames; ++i ) {
    wave_length = ptr->buffer[i] / (ptr->sndfile_info.samplerate);
    avg += abs(wave_length);
    rms += (wave_length * wave_length);
		//printf("%.2d ", wave_length);
	}
  //printf("r: %llu, avg: %d\n", read_frames, avg);

  if (read_frames > 0 && avg > 0) {
    rms /= read_frames;
    rms = sqrt(rms);

    avg /= read_frames;
    //printf("\t%d\n", (int)avg);
//    fflush(stdout);
    if (owave) {
      // write out the wave amps
      fprintf(owave, "\t%.2f\n", rms);
    }
  }

  if (avg > ptr->threshold) {
    //printf("\tnoisy here: %llu until %llu\n", *offset, ((*offset) + read_frames));
    *offset += read_frames; // end of the sequence is possibly silence
    // open the sound file if necessary
    if (!ptr->out_sndfile) {
      silence_detector_create_audio_chunk(ptr);
    }
    // write these bytes to our file 
    sf_writef_int(ptr->out_sndfile, ptr->buffer, read_frames);
  }
  else {
    //printf("starting silence at %llu\n", *offset);
    if (ptr->out_sndfile) {
      sf_close(ptr->out_sndfile);

      ptr->out_sndfile = NULL;
      // get the duration of the chunk and compare to the filter duration
      chunk_duration = read_sound_file_duration(ptr->last_chunk_file_name);
      
      if (verbose ==1) 
         printf("chunk: %s %.2f sec\n", ptr->last_chunk_file_name, chunk_duration);
      if (chunk_duration < ptr->min_duration) {
        
        if (verbose ==1) 
           printf("chunk %s:%.2f is smaller than min duration: %.2f\n", ptr->last_chunk_file_name, chunk_duration, ptr->min_duration);
        unlink(ptr->last_chunk_file_name);
      }
      // the chunk is too long based on the max provided, it will need to be reprocessed with a higher threshold and a smaller window
      if (chunk_duration > ptr->max_duration) {
      
        if (verbose ==1) 
            printf("chunk %s:%.2f is larger than max duration: %.2f\n", ptr->last_chunk_file_name, chunk_duration, ptr->max_duration);
        // reprocess this chunk on it's own using a higher threshold and a smaller buffer
        silence_detector_verify_last_chunk_duration(ptr);
      }
    }
  }

	return read_frames;
}

static void silence_detector_verify_last_chunk_duration(SilenceDetector *ptr) {
  SilenceDetector *nd = silence_detector_new(ptr->last_chunk_file_name, (ptr->buffer_size / 2), (ptr->threshold + 10), ptr->min_duration, ptr->max_duration, NULL);
  silence_detector_split_audio(nd);
  silence_detector_free(nd);
  unlink(ptr->last_chunk_file_name);
  free(ptr->last_chunk_file_name);
  ptr->last_chunk_file_name = NULL;
}


char chunkFile[256];

static int copy_frames_from_to(int *buffer, size_t buffer_size, SNDFILE *src, SNDFILE *dest);

static int update_output_info(const char *name, SF_INFO *output);

int merge(int totalChunks, const char *filename, const char *outputfile) {
	SNDFILE *input, *out = NULL;
	SF_INFO info, outinfo;
  size_t buffer_size = 4096;
  int *buffer = NULL;
  int i = 0;


  memset(&outinfo, 0, sizeof(SF_INFO));
 for(i=0;i<totalChunks;i++)
 {
 sprintf (chunkFile,"%s.chunk%d.wav",filename,i);
 if (update_output_info(chunkFile, &outinfo)) {
      return 1;
    }
}
  if (verbose == 1)
   printf("%s will be %.2f seconds\n", outputfile , ((double)outinfo.frames / (double)outinfo.samplerate));

  out = sf_open(outputfile, SFM_WRITE, &outinfo);

  if (!out) {
    fprintf(stderr, "unable to write file: %s\n", outputfile);
    return 3;
  }

	buffer = (int*)malloc((buffer_size*2) * sizeof(int)); // 2x size we are not sure how the frame is being read/written in libsndfile...

  memset(&info, 0, sizeof(SF_INFO));
 for(i=0;i<totalChunks;i++)
 {
  sprintf (chunkFile,"%s.chunk%d.wav",filename,i);
  
    input = sf_open(chunkFile, SFM_READ, &info);
    if (!input) {
     if (verbose ==1)
        fprintf(stderr, "failed to open input: %s\n", "test.wav");
      //return 4;
    }else 
    {
    copy_frames_from_to(buffer, buffer_size, input, out);
    if (verbose == 1)
      printf("wrote from src: %s\n", chunkFile);
      
    sf_close(input);
    unlink(chunkFile);
    }
}
  free(buffer);
  sf_close(out);

  return 0;
}

// read information about the input file and update the output info
static int update_output_info(const char *name, SF_INFO *output) {
	SNDFILE *input = NULL;
  SF_INFO info;

  memset(&info, 0, sizeof(SF_INFO));

  input = sf_open(name, SFM_READ, &info);
  if (!input) {
    if (verbose ==1 )
     fprintf(stderr, "failed to open input: %s\n", name);
  }else
  {
  if (verbose == 1)
    printf("%s is %.2f seconds\n", name, ((double)info.frames / (double)info.samplerate));
    
  output->frames     = output->frames + info.frames;
  output->samplerate = info.samplerate;
  output->channels   = info.channels;
  output->format     = info.format;
  output->sections   = output->sections + info.sections;
  output->seekable   = info.seekable;

  sf_close(input);
  }
  return 0;
}

static int copy_frames_from_to(int *buffer, size_t buffer_size, SNDFILE *src, SNDFILE *dest) {
	sf_count_t read_frames;

  do {
    memset(buffer, 0, buffer_size * sizeof(int));
    read_frames = sf_readf_int(src, buffer, buffer_size);
    if (read_frames > 0) {
      sf_writef_int(dest, buffer, read_frames);
    }
  } while (read_frames > 0);

  return 0;
}

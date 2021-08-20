#include "h5read.h"

#include <hdf5.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "eiger2xe.h"

uint8_t *mask;
uint8_t *module_mask;
size_t mask_size;

// VDS stuff

#define MAXFILENAME 256
#define MAXDATAFILES 100
#define MAXDIM 3

typedef struct h5_data_file {
    char filename[MAXFILENAME];
    char dsetname[MAXFILENAME];
    hid_t file;
    hid_t dataset;
    size_t frames;
    size_t offset;
} h5_data_file;

struct _h5read_handle {
    int master_file;
    int data_file_count;
    h5_data_file *data_files;
    size_t frames;  ///< Number of frames in this dataset
    size_t slow;    ///< Pixel dimension of images in the slow direction
    size_t fast;    ///< Pixel dimensions of images in the fast direction
};

int data_file_current;

hid_t master;

void h5read_free(h5read_handle *obj) {
    for (int i = 0; i < obj->data_file_count; i++) {
        H5Dclose(obj->data_files[i].dataset);
        H5Fclose(obj->data_files[i].file);
    }
    free(obj->data_files);
    H5Fclose(obj->master_file);

    free(mask);
    free(module_mask);

    free(obj);
}

/// Get the number of frames available
size_t h5read_get_number_of_images(h5read_handle *obj) {
    return obj->frames;
}

size_t h5read_get_image_slow(h5read_handle *obj) {
    return obj->slow;
}

size_t h5read_get_image_fast(h5read_handle *obj) {
    return obj->fast;
}

void h5read_free_image(image_t *i) {
    free(i->data);
    // Mask is a pointer to the file-global file mask so isn't freed
    free(i);
}

/* blit the relevent pixel data across from a single image into an collection
   of image modules - will allocate the latter */
void blit(image_t image, image_modules_t *modules) {
    size_t fast, slow, offset, target;

    if (image.slow == E2XE_16M_SLOW) {
        fast = 4;
        slow = 8;
    } else {
        fast = 2;
        slow = 4;
    }

    modules->mask = module_mask;
    modules->slow = E2XE_MOD_SLOW;
    modules->fast = E2XE_MOD_FAST;
    modules->modules = slow * fast;

    size_t module_pixels = E2XE_MOD_SLOW * E2XE_MOD_FAST;

    modules->data = (uint16_t *)malloc(sizeof(uint16_t) * slow * fast * module_pixels);

    for (size_t _slow = 0; _slow < slow; _slow++) {
        size_t row0 = _slow * (E2XE_MOD_SLOW + E2XE_GAP_SLOW) * image.fast;
        for (size_t _fast = 0; _fast < fast; _fast++) {
            for (size_t row = 0; row < E2XE_MOD_SLOW; row++) {
                offset =
                  (row0 + row * image.fast + _fast * (E2XE_MOD_FAST + E2XE_GAP_FAST));
                target = (_slow * fast + _fast) * module_pixels + row * E2XE_MOD_FAST;
                memcpy((void *)&modules->data[target],
                       (void *)&image.data[offset],
                       sizeof(uint16_t) * E2XE_MOD_FAST);
            }
        }
    }
}

image_modules_t get_image_modules(size_t n) {
    image_t image = get_image(n);
    image_modules_t modules;
    modules.data = NULL;
    modules.mask = NULL;
    modules.modules = -1;
    modules.fast = -1;
    modules.slow = -1;
    blit(image, &modules);
    free_image(image);
    return modules;
}

void free_image_modules(image_modules_t i) {
    free(i.data);
}

image_t *h5read_get_image(h5read_handle *obj, size_t n) {
    if (n >= obj->frames) {
        fprintf(stderr, "image %ld > frames (%ld)\n", n, obj->frames);
        exit(1);
    }

    /* first find the right data file - having to do this lookup is annoying
       but probably cheap */

    int data_file;
    for (data_file = 0; data_file < obj->data_file_count; data_file++) {
        if ((n - obj->data_files[data_file].offset)
            < obj->data_files[data_file].frames) {
            break;
        }
    }

    if (data_file == obj->data_file_count) {
        fprintf(stderr, "could not find data file for frame %ld\n", n);
        exit(1);
    }

    h5_data_file *current = &(obj->data_files[data_file]);

    hid_t space = H5Dget_space(current->dataset);
    hid_t datatype = H5Dget_type(current->dataset);

    hsize_t block[3] = {1, obj->slow, obj->fast};
    hsize_t offset[3] = {n - current->offset, 0, 0};

    // select data to read #todo add status checks
    H5Sselect_hyperslab(space, H5S_SELECT_SET, offset, NULL, block, NULL);
    hid_t mem_space = H5Screate_simple(3, block, NULL);

    uint16_t *buffer = (uint16_t *)malloc(sizeof(uint16_t) * obj->slow * obj->fast);
    if (H5Dread(current->dataset, datatype, mem_space, space, H5P_DEFAULT, buffer)
        < 0) {
        H5Eprint(H5E_DEFAULT, NULL);
        exit(1);
    }

    H5Sclose(space);
    H5Sclose(mem_space);

    image_t *result = malloc(sizeof(image_t));
    result->slow = obj->slow;
    result->fast = obj->fast;
    result->mask = mask;
    result->data = buffer;

    return result;
}

// void free_image_modules(image_modules_t image);

void read_mask() {
    // uses master pointer above: beware if this is bad

    char mask_path[] = "/entry/instrument/detector/pixel_mask";
    hid_t mask_dataset, mask_info, datatype;

    size_t mask_dsize;
    uint32_t *raw_mask;
    uint64_t *raw_mask_64;  // why?

    mask_dataset = H5Dopen(master, mask_path, H5P_DEFAULT);

    if (mask_dataset < 0) {
        fprintf(stderr, "error reading mask from %s\n", mask_path);
        exit(1);
    }

    datatype = H5Dget_type(mask_dataset);
    mask_info = H5Dget_space(mask_dataset);

    mask_dsize = H5Tget_size(datatype);
    if (mask_dsize == 4) {
        printf("mask dtype uint32");
    } else if (mask_dsize == 8) {
        printf("mask dtype uint64");
    } else {
        fprintf(stderr, "mask data size != 4,8 (%ld)\n", H5Tget_size(datatype));
        exit(1);
    }

    mask_size = H5Sget_simple_extent_npoints(mask_info);

    printf("mask has %ld elements\n", mask_size);

    void *buffer = NULL;

    if (mask_dsize == 4) {
        raw_mask = (uint32_t *)malloc(sizeof(uint32_t) * mask_size);
        buffer = (void *)raw_mask;
        raw_mask_64 = NULL;
    } else {
        raw_mask_64 = (uint64_t *)malloc(sizeof(uint64_t) * mask_size);
        buffer = (void *)raw_mask_64;
        raw_mask = NULL;
    }

    if (H5Dread(mask_dataset, datatype, H5S_ALL, H5S_ALL, H5P_DEFAULT, buffer) < 0) {
        fprintf(stderr, "error reading mask\n");
        exit(1);
    }

    // count 0's

    size_t zero = 0;

    mask = (uint8_t *)malloc(sizeof(uint8_t) * mask_size);

    if (mask_dsize == 4) {
        for (size_t j = 0; j < mask_size; j++) {
            if (raw_mask[j] == 0) {
                zero++;
                mask[j] = 1;
            } else {
                mask[j] = 0;
            }
        }
    } else {
        for (size_t j = 0; j < mask_size; j++) {
            if (raw_mask_64[j] == 0) {
                zero++;
                mask[j] = 1;
            } else {
                mask[j] = 0;
            }
        }
    }

    // blit mask over to module mask

    size_t fast, slow, offset, target, image_slow, image_fast, module_pixels;
    module_pixels = E2XE_MOD_FAST * E2XE_MOD_SLOW;

    if (mask_size == E2XE_16M_SLOW * E2XE_16M_FAST) {
        slow = 8;
        fast = 4;
        image_slow = E2XE_16M_SLOW;
        image_fast = E2XE_16M_FAST;
    } else {
        slow = 4;
        fast = 2;
        image_slow = E2XE_4M_SLOW;
        image_fast = E2XE_4M_FAST;
    }
    module_mask = (uint8_t *)malloc(sizeof(uint8_t) * fast * slow * module_pixels);
    for (size_t _slow = 0; _slow < slow; _slow++) {
        size_t row0 = _slow * (E2XE_MOD_SLOW + E2XE_GAP_SLOW) * image_fast;
        for (size_t _fast = 0; _fast < fast; _fast++) {
            for (size_t row = 0; row < E2XE_MOD_SLOW; row++) {
                offset =
                  (row0 + row * image_fast + _fast * (E2XE_MOD_FAST + E2XE_GAP_FAST));
                target = (_slow * fast + _fast) * module_pixels + row * E2XE_MOD_FAST;
                memcpy((void *)&module_mask[target],
                       (void *)&mask[offset],
                       sizeof(uint8_t) * E2XE_MOD_FAST);
            }
        }
    }

    printf("%ld of the pixels are valid\n", zero);

    // cleanup

    if (raw_mask) free(raw_mask);
    if (raw_mask_64) free(raw_mask_64);
    H5Dclose(mask_dataset);
}

/// Get number of VDS and read info about all the sub-files.
///
/// @param master           HDF5 File object pointing to the master file
/// @param dataset          The root dataset to search for VDS from
/// @param data_files_array Pointer to an array variable, that will be
///                         allocated and filled with basic information
///                         about the VDS sub-files.
/// @returns The number of VDS found and allocated into data_files_array
int vds_info(char *root, hid_t master, hid_t dataset, h5_data_file **data_files_array) {
    hid_t plist, vds_source;
    size_t vds_count;
    herr_t status;

    plist = H5Dget_create_plist(dataset);

    status = H5Pget_virtual_count(plist, &vds_count);

    *data_files_array = calloc(vds_count, sizeof(h5_data_file));
    // Used to use vds parameter directly - put here so no mass-changes
    h5_data_file *vds = *data_files_array;

    for (int j = 0; j < vds_count; j++) {
        hsize_t start[MAXDIM], stride[MAXDIM], count[MAXDIM], block[MAXDIM];
        size_t dims;

        vds_source = H5Pget_virtual_vspace(plist, j);
        dims = H5Sget_simple_extent_ndims(vds_source);

        if (dims != 3) {
            H5Sclose(vds_source);
            fprintf(stderr, "incorrect data dimensionality: %d\n", (int)dims);
            return -1;
        }

        H5Sget_regular_hyperslab(vds_source, start, stride, count, block);
        H5Sclose(vds_source);

        H5Pget_virtual_filename(plist, j, vds[j].filename, MAXFILENAME);
        H5Pget_virtual_dsetname(plist, j, vds[j].dsetname, MAXFILENAME);

        for (int k = 1; k < dims; k++) {
            if (start[k] != 0) {
                fprintf(stderr, "incorrect chunk start: %d\n", (int)start[k]);
                return -1;
            }
        }

        vds[j].frames = block[0];
        vds[j].offset = start[0];

        if ((strlen(vds[j].filename) == 1) && (vds[j].filename[0] == '.')) {
            H5L_info_t info;
            status = H5Lget_info(master, vds[j].dsetname, &info, H5P_DEFAULT);

            if (status) {
                fprintf(stderr, "error from H5Lget_info on %s\n", vds[j].dsetname);
                return -1;
            }

            /* if the data file points to an external source, dereference */

            if (info.type == H5L_TYPE_EXTERNAL) {
                char buffer[MAXFILENAME], scr[MAXFILENAME];
                unsigned flags;
                const char *nameptr, *dsetptr;

                H5Lget_val(master, vds[j].dsetname, buffer, MAXFILENAME, H5P_DEFAULT);
                H5Lunpack_elink_val(
                  buffer, info.u.val_size, &flags, &nameptr, &dsetptr);

                /* assumptions herein:
                    - external link references are local paths
                    - only need to worry about UNIX paths e.g. pathsep is /
                    - ASCII so chars are ... chars
                   so manually assemble...
                 */

                strcpy(scr, root);
                scr[strlen(root)] = '/';
                strcpy(scr + strlen(root) + 1, nameptr);

                strcpy(vds[j].filename, scr);
                strcpy(vds[j].dsetname, dsetptr);
            }
        } else {
            char scr[MAXFILENAME];
            sprintf(scr, "%s/%s", root, vds[j].filename);
            strcpy(vds[j].filename, scr);
        }

        // do I want to open these here? Or when they are needed...
        vds[j].file = 0;
        vds[j].dataset = 0;
    }

    status = H5Pclose(plist);

    return vds_count;
}

/// Extracts the h5_data_file dictionary for information on all VDS
///
/// @param filename         The name of the master file
/// @param h5_data_file     The data_files array to be allocated and filled
///
/// @returns The number of VDS files
int unpack_vds(const char *filename, h5_data_file **data_files) {
    // TODO if we want this to become SWMR aware in the future will need to
    // allow for that here
    hid_t file = H5Fopen(filename, H5F_ACC_RDONLY, H5P_DEFAULT);

    if (file < 0) {
        fprintf(stderr, "error reading %s\n", filename);
        return -1;
    }

    hid_t dataset = H5Dopen(file, "/entry/data/data", H5P_DEFAULT);

    if (dataset < 0) {
        H5Fclose(file);
        fprintf(stderr, "error reading %s\n", "/entry/data/data");
        return -1;
    }

    /* always set the absolute path to file information */
    char rootpath[MAXFILENAME];
    strncpy(rootpath, filename, MAXFILENAME);
    char *root = dirname(rootpath);
    char cwd[MAXFILENAME];
    if ((strlen(root) == 1) && (root[0] == '.')) {
        root = getcwd(cwd, MAXFILENAME);
    }

    int vds_count = vds_info(root, file, dataset, data_files);

    H5Dclose(dataset);
    H5Fclose(file);

    return vds_count;
}

void setup_data(h5read_handle *obj) {
    hid_t dataset = obj->data_files[0].dataset;
    hid_t datatype = H5Dget_type(dataset);

    if (H5Tget_size(datatype) != 2) {
        fprintf(stderr, "native data size != 2 (%ld)\n", H5Tget_size(datatype));
        exit(1);
    }

    hid_t space = H5Dget_space(dataset);

    if (H5Sget_simple_extent_ndims(space) != 3) {
        fprintf(stderr, "raw data not three dimensional\n");
        exit(1);
    }

    hsize_t dims[3];
    H5Sget_simple_extent_dims(space, dims, NULL);

    obj->slow = dims[1];
    obj->fast = dims[2];

    printf("Total data size: %ldx%ldx%ld\n", obj->frames, obj->slow, obj->fast);
    H5Sclose(space);
}

h5read_handle *h5read_open(const char *master_filename) {
    /* I'll do my own debug printing: disable HDF5 library output */
    H5Eset_auto(H5E_DEFAULT, NULL, NULL);

    int master_file = H5Fopen(master_filename, H5F_ACC_RDONLY, H5P_DEFAULT);

    if (master_file < 0) {
        fprintf(stderr, "error reading %s\n", master_filename);
        return NULL;
    }

    // Create the H5 handle object
    h5read_handle *file = calloc(1, sizeof(h5read_handle));
    file->master_file = master_file;

    file->data_file_count = unpack_vds(master_filename, &file->data_files);

    if (file->data_file_count < 0) {
        fprintf(stderr, "error reading %s\n", master_filename);
        H5Fclose(master_file);
        free(file);
        return NULL;
    }

    // open up the actual data files, count all the frames
    file->frames = 0;
    h5_data_file *data_files = file->data_files;
    for (int j = 0; j < file->data_file_count; j++) {
        data_files[j].file =
          H5Fopen(data_files[j].filename, H5F_ACC_RDONLY, H5P_DEFAULT);
        if (data_files[j].file < 0) {
            fprintf(stderr, "error reading %s\n", data_files[j].filename);
            // Lots of code to cleanup here, so just quit for now
            exit(1);
        }
        data_files[j].dataset =
          H5Dopen(data_files[j].file, data_files[j].dsetname, H5P_DEFAULT);
        if (data_files[j].dataset < 0) {
            fprintf(stderr, "error reading %s\n", data_files[j].filename);
            // Lots of code to cleanup here, so just quit for now
            exit(1);
        }
        file->frames += data_files[j].frames;
    }

    read_mask();

    setup_data(file);

    return NULL;
}

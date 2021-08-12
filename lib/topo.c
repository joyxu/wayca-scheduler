/*
 * Copyright (c) 2021 HiSilicon Technologies Co., Ltd.
 * Wayca scheduler is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 *
 * See the Mulan PSL v2 for more details.
 */

/* topo.c
 * Author: Guodong Xu <guodong.xu@linaro.org>
 */

#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include "common.h"
#include "topo.h"
#include "log.h"
#include "wayca-scheduler.h"

WAYCA_SC_INIT_PRIO(topo_init, TOPO);
WAYCA_SC_FINI_PRIO(topo_free, TOPO);
static struct wayca_topo topo;

/* topo_path_read_buffer - read from filename into buf, maximum 'count' in size
 * return:
 *   negative on error
 *   0 or more: total number of bytes read
 */
static int topo_path_read_buffer(const char *base, const char *filename,
				 char *buf, size_t count)
{
	int dir_fd;
	int fd;
	FILE *f;
	int ret;
	int c = 0, tries = 0;

	dir_fd = open(base, O_RDONLY | O_CLOEXEC);
	if (dir_fd == -1)
		return -errno;

	fd = openat(dir_fd, filename, O_RDONLY);
	if (fd == -1) {
		ret = errno;
		close(dir_fd);
		return -ret;
	}

	f = fdopen(fd, "r");
	if (f == NULL) {
		ret = errno;
		close(fd);
		close(dir_fd);
		return -ret;
	}

	memset(buf, 0, count);
	while (count > 0) {
		ret = read(fd, buf, count);
		if (ret < 0) {
			if ((errno == EAGAIN || errno == EINTR) &&
			    (tries++ < WAYCA_SC_MAX_FD_RETRIES)) {
				usleep(WAYCA_SC_USLEEP_DELAY_250MS);
				continue;
			}
			c = c ? c : (-errno);
			break;
		}
		if (ret == 0)
			break;
		tries = 0;
		count -= ret;
		buf += ret;
		c += ret;
	}

	fclose(f);
	close(fd);
	close(dir_fd);

	return c;
}

/* return negative on error, 0 on success */
static int topo_path_read_s32(const char *base, const char *filename,
			      int *result)
{
	int dir_fd;
	int fd;
	FILE *f;
	int ret, t;

	dir_fd = open(base, O_RDONLY | O_CLOEXEC);
	if (dir_fd == -1)
		return -errno;

	fd = openat(dir_fd, filename, O_RDONLY);
	if (fd == -1) {
		ret = errno;
		close(dir_fd);
		return -ret;
	}

	f = fdopen(fd, "r");
	if (f == NULL) {
		ret = errno;
		close(fd);
		close(dir_fd);
		return -ret;
	}

	ret = fscanf(f, "%d", &t);

	fclose(f);
	close(fd);
	close(dir_fd);

	if (ret != 1)
		return -EINVAL;
	if (result)
		*result = t;
	return 0;
}

/*
 * topo_path_read_multi_s32 - read nmemb s32 integer from the given filename
 *  - array: is a pre-allocated integer array
 *  - nmemb: number of members to read from file "base/filename"
 *
 * return negative on error, 0 on success
 */
static int topo_path_read_multi_s32(const char *base, const char *filename,
				    size_t nmemb, int array[])
{
	int dir_fd;
	int fd;
	FILE *f;
	int i, t;
	int ret = 0;

	dir_fd = open(base, O_RDONLY | O_CLOEXEC);
	if (dir_fd == -1)
		return -errno;

	fd = openat(dir_fd, filename, O_RDONLY);
	if (fd == -1) {
		ret = errno;
		close(dir_fd);
		return -ret;
	}

	f = fdopen(fd, "r");
	if (f == NULL) {
		ret = errno;
		close(fd);
		close(dir_fd);
		return -ret;
	}

	for (i = 0; i < nmemb; i++) {
		if (fscanf(f, "%d", &t) != 1) {
			ret = -EINVAL;
			break;
		}
		array[i] = t;
	}

	fclose(f);
	close(fd);
	close(dir_fd);

	return ret;
}

/*
 * topo_path_parse_meminfo - parse 'meminfo'
 *  - filename: usually, this is 'meminfo'
 *  - p_meminfo: a pre-allocated space to store parsing results
 *
 * return negative on error, 0 on success
 */
static int topo_path_parse_meminfo(const char *base, const char *filename,
				   struct wayca_meminfo *p_meminfo)
{
	int dir_fd;
	int fd;
	FILE *f;
	char buf[BUFSIZ];
	char *ptr;
	int ret = -1;

	dir_fd = open(base, O_RDONLY | O_CLOEXEC);
	if (dir_fd == -1)
		return -errno;

	fd = openat(dir_fd, filename, O_RDONLY);
	if (fd == -1) {
		ret = errno;
		close(dir_fd);
		return -ret;
	}

	f = fdopen(fd, "r");
	if (f == NULL) {
		ret = errno;
		close(fd);
		close(dir_fd);
		return -ret;
	}

	while (fgets(buf, sizeof(buf), f) != NULL) {
		ptr = strstr(buf, "MemTotal:");
		if (ptr == NULL)
			continue;

		if (sscanf(ptr, "%*s %lu", &p_meminfo->total_avail_kB) != 1)
			break;
		ret = 0;
		break;
	}

	fclose(f);
	close(fd);
	close(dir_fd);

	return ret;
}

/* Note: cpuset_nbits(), nextnumber(), nexttoken(), cpulist_parse() are referenced
 *       from https://github.com/karelzak/util-linux
 */
#define cpuset_nbits(setsize) (8 * (setsize))

static const char *nexttoken(const char *q, int sep)
{
	if (q)
		q = strchr(q, sep);
	if (q)
		q++;
	return q;
}

static int nextnumber(const char *str, char **end, unsigned int *result)
{
	errno = 0;
	if (str == NULL || *str == '\0' || !isdigit(*str))
		return -EINVAL;
	*result = (unsigned int)strtoul(str, end, 10);
	if (errno)
		return -errno;
	if (str == *end)
		return -EINVAL;
	return 0;
}

/*
 * Parses string with list of CPU ranges.
 * Returns 0 on success.
 * Returns 1 on error.
 * Returns 2 if fail is set and a cpu number passed in the list doesn't fit
 * into the cpu_set. If fail is not set cpu numbers that do not fit are
 * ignored and 0 is returned instead.
 */
int cpulist_parse(const char *str, cpu_set_t *set, size_t setsize, int fail)
{
	size_t max = cpuset_nbits(setsize);
	const char *p, *q;
	char *end = NULL;

	q = str;
	CPU_ZERO_S(setsize, set);

	while (p = q, q = nexttoken(q, ','), p) {
		unsigned int a; /* beginning of range */
		unsigned int b; /* end of range */
		unsigned int s; /* stride */
		const char *c1, *c2;

		if (nextnumber(p, &end, &a) != 0)
			return 1;
		b = a;
		s = 1;
		p = end;

		c1 = nexttoken(p, '-');
		c2 = nexttoken(p, ',');

		if (c1 != NULL && (c2 == NULL || c1 < c2)) {
			if (nextnumber(c1, &end, &b) != 0)
				return 1;

			c1 = end && *end ? nexttoken(end, ':') : NULL;

			if (c1 != NULL && (c2 == NULL || c1 < c2)) {
				if (nextnumber(c1, &end, &s) != 0)
					return 1;
				if (s == 0)
					return 1;
			}
		}

		if (!(a <= b))
			return 1;
		while (a <= b) {
			if (fail && (a >= max))
				return 2;
			CPU_SET_S(a, setsize, set);
			a += s;
		}
	}

	if (end && *end)
		return 1;
	return 0;
}

/* return negative value on error, 0 on success */
/* cpu_set_t *set: can contain anything and will be zero'ed by cpulist_parse() */
static int topo_path_read_cpulist(const char *base, const char *filename,
				  cpu_set_t *set, int maxcpus)
{
	size_t len = maxcpus * 8; /* big enough to hold a CPU ids */
	char buf[len]; /* dynamic allocation */
	int ret = 0;
	int dir_fd;
	FILE *f;
	int fd;

	dir_fd = open(base, O_RDONLY | O_CLOEXEC);
	if (dir_fd == -1)
		return -errno;

	fd = openat(dir_fd, filename, O_RDONLY);
	if (fd == -1) {
		close(dir_fd);
		return -errno;
	}

	f = fdopen(fd, "r");
	if (f == NULL) {
		close(fd);
		close(dir_fd);
		return -errno;
	}

	if (fgets(buf, len, f) == NULL)
		ret = -errno;

	fclose(f);
	close(fd);
	close(dir_fd);

	if (ret != 0)
		return ret;

	len = strlen(buf);
	if (buf[len - 1] == '\n')
		buf[len - 1] = '\0';

	if (cpulist_parse(buf, set, CPU_ALLOC_SIZE(maxcpus), 0))
		return -EINVAL;

	return 0;
}

/* topo_read_cpu_topology() - read cpu%d topoloy, where %d is cpu_index
 *
 * Return negative on error, 0 on success
 */
static int topo_read_cpu_topology(int cpu_index, struct wayca_topo *p_topo)
{
	static int max_node_index = -1; /* actual node_index starts from 0 */

	DIR *dir;
	struct dirent *dirent;
	long node_index;
	char *endptr;
	char path_buffer[WAYCA_SC_PATH_LEN_MAX];

	int core_id, cluster_id, ppkg_id;
	int ret;
	int i;

	/* allocate a new struct wayca_cpu */
	p_topo->cpus[cpu_index] =
		(struct wayca_cpu *)calloc(1, sizeof(struct wayca_cpu));
	if (!p_topo->cpus[cpu_index])
		return -ENOMEM;

	p_topo->cpus[cpu_index]->cpu_id = cpu_index;

	sprintf(path_buffer, "%s/cpu%d", WAYCA_SC_CPU_FNAME, cpu_index);

	/* read cpu%d/node* to learn which numa node this cpu belongs to */
	dir = opendir(path_buffer);
	if (!dir)
		return -errno;

	while ((dirent = readdir(dir)) != NULL) {
		if (strncmp(dirent->d_name, "node", 4))
			continue;
		/* read node_index */
		node_index = strtol(dirent->d_name + 4, &endptr, 0);
		/* the right format is "node[0-9]" */
		if (endptr == dirent->d_name + 4)
			continue;
		/* check whether need more node space */
		if (node_index > max_node_index) {
			struct wayca_node **p_temp;

			max_node_index = node_index;
			/* re-allocate for more space */
			p_temp = (struct wayca_node **)realloc(
				p_topo->nodes, (max_node_index + 1) *
				sizeof(struct wayca_node *));
			if (!p_temp) {
				closedir(dir);
				return -ENOMEM;
			}
			p_topo->nodes = p_temp;
		}
		/*
		 * check this 'node_index' node exist or not if not, create one
		 * TODO: refactor wayca_node_create into a new function
		 */
		if (!CPU_ISSET_S(node_index, CPU_ALLOC_SIZE(p_topo->n_cpus),
				 p_topo->node_map)) {
			p_topo->nodes[node_index] = (struct wayca_node *)calloc(
				1, sizeof(struct wayca_node));
			if (!p_topo->nodes[node_index]) {
				closedir(dir);
				return -ENOMEM;
			}
			p_topo->nodes[node_index]->node_idx = node_index;
			/* initialize this node's cpu_map */
			p_topo->nodes[node_index]->cpu_map =
				CPU_ALLOC(p_topo->kernel_max_cpus);
			if (!p_topo->nodes[node_index]->cpu_map) {
				closedir(dir);
				return -ENOMEM;
			}
			CPU_ZERO_S(p_topo->setsize,
				   p_topo->nodes[node_index]->cpu_map);
			/* add node_index into the top-level node map */
			CPU_SET_S(node_index, CPU_ALLOC_SIZE(p_topo->n_cpus),
				  p_topo->node_map);
			p_topo->n_nodes++;
		}
		/* add current CPU into this node's cpu map */
		CPU_SET_S(cpu_index, p_topo->setsize,
			  p_topo->nodes[node_index]->cpu_map);
		p_topo->nodes[node_index]->n_cpus++;
		/* link this node back to current CPU */
		p_topo->cpus[cpu_index]->p_numa_node =
			p_topo->nodes[node_index];
		break; /* found one "node" entry, no need to check any more */
	}
	closedir(dir);

	/* move the base to "cpu%d/topology" */
	sprintf(path_buffer, "%s/cpu%d/topology", WAYCA_SC_CPU_FNAME,
		cpu_index);

	/* read "core_id" */
	if (topo_path_read_s32(path_buffer, "core_id", &core_id) != 0)
		p_topo->cpus[cpu_index]->core_id = -1;
	p_topo->cpus[cpu_index]->core_id = core_id;

	/* read "cluster_id" */
	if (topo_path_read_s32(path_buffer, "cluster_id", &cluster_id) != 0) {
		p_topo->cpus[cpu_index]->p_cluster = NULL;
		goto read_package_info;
	}

	/* check this "cluster_id" exists or not */
	for (i = 0; i < p_topo->n_clusters; i++) {
		if (p_topo->ccls[i]->cluster_id == cluster_id)
			break;
	}
	/* need to create a new wayca_cluster */
	if (i == p_topo->n_clusters) {
		struct wayca_cluster **p_temp;
		/* TODO: refactor this into a new funcion */
		p_topo->n_clusters++;
		/* increase p_topo->ccls array */
		p_temp = (struct wayca_cluster **)realloc(
			p_topo->ccls,
			(p_topo->n_clusters) * sizeof(struct wayca_cluster *));
		if (!p_temp)
			return -ENOMEM;
		p_topo->ccls = p_temp;
		/* allocate a new wayca_cluster struct, and link it to p_topo->ccls */
		p_topo->ccls[i] = (struct wayca_cluster *)calloc(
			1, sizeof(struct wayca_cluster));
		if (!p_topo->ccls[i])
			return -ENOMEM;
		/* initialize this cluster's cpu_map */
		p_topo->ccls[i]->cpu_map = CPU_ALLOC(p_topo->kernel_max_cpus);
		if (!p_topo->ccls[i]->cpu_map)
			return -ENOMEM;
		/* read "cluster_cpus_list" */
		ret = topo_path_read_cpulist(path_buffer, "cluster_cpus_list",
					     p_topo->ccls[i]->cpu_map,
					     p_topo->kernel_max_cpus);
		if (ret) {
			PRINT_ERROR(
				"get ccl %d cluster_cpu_list fail, ret = %d\n",
				i, ret);
			return ret;
		}
		/* assign cluster_id and n_cpus */
		p_topo->ccls[i]->cluster_id = cluster_id;
		p_topo->ccls[i]->n_cpus =
			CPU_COUNT_S(p_topo->setsize, p_topo->ccls[i]->cpu_map);
	}
	/* link this cluster back to current CPU */
	p_topo->cpus[cpu_index]->p_cluster = p_topo->ccls[i];

read_package_info:
	/* read "physical_package_id" */
	ret = topo_path_read_s32(path_buffer, "physical_package_id", &ppkg_id);
	if (ret) {
		PRINT_ERROR("get physical_package_id fail, ret = %d\n", ret);
		return ret;
	}

	/* check this "physical_package_id" exists or not */
	for (i = 0; i < p_topo->n_packages; i++)
		if (p_topo->packages[i]->physical_package_id == ppkg_id)
			break;
	/* need to create a new wayca_package */
	if (i == p_topo->n_packages) {
		struct wayca_package **p_temp;
		/* TODO: refactor this into a new funcion */
		p_topo->n_packages++;
		/* increase p_topo->packages array */
		p_temp = (struct wayca_package **)realloc(
			p_topo->packages,
			p_topo->n_packages * sizeof(struct wayca_package *));
		if (!p_temp)
			return -ENOMEM;
		p_topo->packages = p_temp;
		/* allocate a new wayca_ package struct, and link it to p_topo->ccls */
		p_topo->packages[i] = (struct wayca_package *)calloc(
			1, sizeof(struct wayca_package));
		if (!p_topo->packages[i])
			return -ENOMEM;
		/* initialize this package's cpu_map */
		p_topo->packages[i]->cpu_map =
			CPU_ALLOC(p_topo->kernel_max_cpus);
		if (!p_topo->packages[i]->cpu_map)
			return -ENOMEM;
		/* initialize this package's numa_map */
		p_topo->packages[i]->numa_map = CPU_ALLOC(p_topo->n_cpus);
		if (!p_topo->packages[i]->numa_map)
			return -ENOMEM;
		CPU_ZERO_S(CPU_ALLOC_SIZE(p_topo->n_cpus),
			   p_topo->packages[i]->numa_map);
		/* read "package_cpus_list" */
		ret = topo_path_read_cpulist(path_buffer, "package_cpus_list",
					     p_topo->packages[i]->cpu_map,
					     p_topo->kernel_max_cpus);
		if (ret) {
			PRINT_ERROR(
				"get package %d package_cpu_list fail, ret = %d\n",
				i, ret);
			return ret;
		}
		/* assign physical_package_id and n_cpus */
		p_topo->packages[i]->physical_package_id = ppkg_id;
		p_topo->packages[i]->n_cpus = CPU_COUNT_S(
			p_topo->setsize, p_topo->packages[i]->cpu_map);
	}
	/* link this package back to current CPU */
	p_topo->cpus[cpu_index]->p_package = p_topo->packages[i];

	/* read core_cpus_list, (SMT: simultaneous multi-threading) */
	p_topo->cpus[cpu_index]->core_cpus_map =
		CPU_ALLOC(p_topo->kernel_max_cpus);
	if (!p_topo->cpus[cpu_index]->core_cpus_map)
		return -ENOMEM;
	/* read "core_cpus_list" */
	ret = topo_path_read_cpulist(path_buffer, "core_cpus_list",
				     p_topo->cpus[cpu_index]->core_cpus_map,
				     p_topo->kernel_max_cpus);
	if (ret) {
		PRINT_ERROR("get cpu %d core_cpus_list fail, ret = %d\n",
			    cpu_index, ret);
		return ret;
	}

	/* read caches information */
	size_t n_caches = 0;
	struct wayca_cache *p_caches;
	int type_len, real_len;

	/* count the number of caches exists */
	do {
		/* move the base to "cpu%d/cache/index%zu" */
		sprintf(path_buffer, "%s/cpu%d/cache/index%zu",
			WAYCA_SC_CPU_FNAME, cpu_index, n_caches);
		/* check access */
		if (access(path_buffer, F_OK) != 0) /* doesn't exist */
			break;
		n_caches++;
	} while (1);
	p_topo->cpus[cpu_index]->n_caches = n_caches;

	if (n_caches == 0) {
		PRINT_DBG("no cache exists for CPU %d", cpu_index);
		return 0;
	}

	/* allocate wayca_cache matrix */
	p_topo->cpus[cpu_index]->p_caches = (struct wayca_cache *)calloc(
		n_caches, sizeof(struct wayca_cache));
	if (!p_topo->cpus[cpu_index]->p_caches)
		return -ENOMEM;

	p_caches = p_topo->cpus[cpu_index]->p_caches;

	for (i = 0; i < n_caches; i++) {
		/* move the base to "cpu%d/cache/index%zu" */
		sprintf(path_buffer, "%s/cpu%d/cache/index%d",
			WAYCA_SC_CPU_FNAME, cpu_index, i);

		/* read cache: id, level, type. default set to -1 on failure */
		if (topo_path_read_s32(path_buffer, "id", &p_caches[i].id) != 0)
			p_caches[i].id = -1;
		if (topo_path_read_s32(path_buffer, "level",
				       &p_caches[i].level) != 0)
			p_caches[i].level = -1;
		type_len = topo_path_read_buffer(path_buffer, "type",
						 p_caches[i].type,
						 WAYCA_SC_ATTR_STRING_LEN);
		real_len = strlen(p_caches[i].type);

		/* remove trailing newline and nonsense chars on success */
		if (type_len <= 0)
			p_caches[i].type[0] = '\0';
		else if (p_caches[i].type[real_len - 1] == '\n')
			p_caches[i].type[real_len - 1] = '\0';

		/* read cache: allocation_policy */
		type_len =
			topo_path_read_buffer(path_buffer, "allocation_policy",
					      p_caches[i].allocation_policy,
					      WAYCA_SC_ATTR_STRING_LEN);
		real_len = strlen(p_caches[i].allocation_policy);
		if (type_len <= 0)
			p_caches[i].allocation_policy[0] = '\0';
		else if (p_caches[i].allocation_policy[real_len - 1] == '\n')
			p_caches[i].allocation_policy[real_len - 1] = '\0';

		/* read cache: write_policy */
		type_len = topo_path_read_buffer(path_buffer, "write_policy",
						 p_caches[i].write_policy,
						 WAYCA_SC_ATTR_STRING_LEN);
		real_len = strlen(p_caches[i].write_policy);
		if (type_len <= 0)
			p_caches[i].write_policy[0] = '\0';
		else if (p_caches[i].write_policy[real_len - 1] == '\n')
			p_caches[i].write_policy[real_len - 1] = '\0';

		/* read cache: ways_of_associativity, etc. */
		topo_path_read_s32(path_buffer, "ways_of_associativity",
				   (int *)&p_caches[i].ways_of_associativity);
		topo_path_read_s32(path_buffer, "physical_line_partition",
				   (int *)&p_caches[i].physical_line_partition);
		topo_path_read_s32(path_buffer, "number_of_sets",
				   (int *)&p_caches[i].number_of_sets);
		topo_path_read_s32(path_buffer, "coherency_line_size",
				   (int *)&p_caches[i].coherency_line_size);

		/* read cache size */
		type_len = topo_path_read_buffer(path_buffer, "size",
						 p_caches[i].cache_size,
						 WAYCA_SC_ATTR_STRING_LEN);
		real_len = strlen(p_caches[i].cache_size);
		if (type_len <= 0)
			p_caches[i].cache_size[0] = '\0';
		/* TODO: parse size from string to unsigned long int */
		else if (p_caches[i].cache_size[real_len - 1] == '\n')
			p_caches[i].cache_size[real_len - 1] = '\0';

		/* read cache: shared_cpu_list */
		p_caches[i].shared_cpu_map = CPU_ALLOC(p_topo->kernel_max_cpus);
		if (!p_caches[i].shared_cpu_map)
			return -ENOMEM;
		ret = topo_path_read_cpulist(path_buffer, "shared_cpu_list",
					     p_caches[i].shared_cpu_map,
					     p_topo->kernel_max_cpus);
		if (ret) {
			PRINT_ERROR("Failed to read %s/shared_cpu_list, Error code: %d\n",
				    path_buffer, ret);
			return ret;
		}
	}

	return 0;
}

/* topo_read_node_topology() - read node%d topoloy, where %d is node_index
 *
 * Return negative on error, 0 on success
 */
static int topo_read_node_topology(int node_index, struct wayca_topo *p_topo)
{
	cpu_set_t *node_cpu_map;
	char path_buffer[WAYCA_SC_PATH_LEN_MAX];
	struct wayca_meminfo *meminfo_tmp;
	int *distance_array;
	int ret;

	sprintf(path_buffer, "%s/node%d", WAYCA_SC_NODE_FNAME, node_index);

	/* read node's cpulist */
	node_cpu_map = CPU_ALLOC(p_topo->kernel_max_cpus);
	if (!node_cpu_map)
		return -ENOMEM;

	ret = topo_path_read_cpulist(path_buffer, "cpulist", node_cpu_map,
				     p_topo->kernel_max_cpus);
	if (ret)
		return ret;
	/* check w/ what's previously composed in cpu_topology reading */
	if (!CPU_EQUAL_S(p_topo->setsize, node_cpu_map,
			 p_topo->nodes[node_index]->cpu_map)) {
		PRINT_ERROR("mismatch detected in node%d cpulist read",
			    node_index);
		return -EINVAL;
	}
	CPU_FREE(node_cpu_map);

	/* allocate a distance array*/
	distance_array = (int *)calloc(p_topo->n_nodes, sizeof(int));
	if (!distance_array)
		return -ENOMEM;
	/* read node's distance */
	ret = topo_path_read_multi_s32(path_buffer, "distance", p_topo->n_nodes,
				       distance_array);
	if (ret) {
		PRINT_ERROR("get node distance fail, ret = %d\n", ret);
		free(distance_array);
		return ret;
	}

	p_topo->nodes[node_index]->distance = distance_array;

	/* read meminfo */
	meminfo_tmp =
		(struct wayca_meminfo *)calloc(1, sizeof(struct wayca_meminfo));
	if (!meminfo_tmp)
		return -ENOMEM;
	ret = topo_path_parse_meminfo(path_buffer, "meminfo", meminfo_tmp);
	if (ret) {
		PRINT_ERROR("get node meminfo fail, ret = %d\n", ret);
		free(meminfo_tmp);
		return ret;
	}

	p_topo->nodes[node_index]->p_meminfo = meminfo_tmp;

	return 0;
}

/* topo_construct_core_topology
 *  This function takes in wayca_cpus information and construct a wayca_cores
 *  topology.
 * Return negative on error, 0 on success
 */
static int topo_construct_core_topology(struct wayca_topo *p_topo)
{
	struct wayca_core **pp_core;
	int cur_core_id;
	int i, j;

	/* initialization */
	if (p_topo->cores != NULL || p_topo->n_cores != 0) {
		WAYCA_SC_LOG_ERR(
			"Duplicated call, wayca_cores has been established\n");
		return -1;
	}

	/* go through all n_cpus */
	for (i = 0; i < p_topo->n_cpus; i++) {
		/* read this cpu's core_id */
		cur_core_id = p_topo->cpus[i]->core_id;
		/* check whether it is already in wayca_core array */
		for (j = 0; j < p_topo->n_cores; j++) {
			if (p_topo->cores[j]->core_id == cur_core_id)
				break; /* it exists, skip */
		}
		/*
		 * cur_core_id exists in p_topo->cores, just go to check
		 * next one
		 */
		if (j < p_topo->n_cores)
			continue;

		/* allocate a new wayca_core if cur_core_id inexist */
		pp_core = (struct wayca_core **)realloc(
			p_topo->cores,
			(p_topo->n_cores + 1) * sizeof(struct wayca_core *));
		if (!pp_core)
			return -ENOMEM;
		p_topo->cores = pp_core;

		/* j equals p_topo->n_cores */
		p_topo->cores[j] = (struct wayca_core *)calloc(
			1, sizeof(struct wayca_core));
		if (!p_topo->cores[j])
			return -ENOMEM;

		/* increase p_topo->n_cores */
		p_topo->n_cores++;

		/* copy from cpus[i] to cores[j] */
		p_topo->cores[j]->core_id = cur_core_id;
		p_topo->cores[j]->core_cpus_map =
			p_topo->cpus[i]->core_cpus_map;
		p_topo->cores[j]->n_cpus = CPU_COUNT_S(
			p_topo->setsize, p_topo->cores[j]->core_cpus_map);
		p_topo->cores[j]->p_cluster = p_topo->cpus[i]->p_cluster;
		p_topo->cores[j]->p_numa_node = p_topo->cpus[i]->p_numa_node;
		p_topo->cores[j]->p_package = p_topo->cpus[i]->p_package;
		p_topo->cores[j]->n_caches = p_topo->cpus[i]->n_caches;
		p_topo->cores[j]->p_caches = p_topo->cpus[i]->p_caches;
	}
	return 0;
}

static int topo_recursively_read_io_devices(const char *rootdir,
					    struct wayca_topo *p_topo);

/* External callable functions */
static void topo_init(void)
{
	char origin_wd[WAYCA_SC_PATH_LEN_MAX];
	struct wayca_topo *p_topo = &topo;
	cpu_set_t *cpuset_possible;
	cpu_set_t *node_possible;
	int i = 0;
	int ret;

	getcwd(origin_wd, WAYCA_SC_PATH_LEN_MAX);
	memset(p_topo, 0, sizeof(struct wayca_topo));

	/*
	 * read "cpu/kernel_max" to determine maximum size for future memory
	 * allocations
	 */
	if (topo_path_read_s32(WAYCA_SC_CPU_FNAME, "kernel_max",
			       &p_topo->kernel_max_cpus) == 0)
		p_topo->kernel_max_cpus += 1;
	else
		p_topo->kernel_max_cpus = WAYCA_SC_DEFAULT_KERNEL_MAX;
	p_topo->setsize = CPU_ALLOC_SIZE(p_topo->kernel_max_cpus);

	/* read "cpu/possible" to determine number of CPUs */
	cpuset_possible = CPU_ALLOC(p_topo->kernel_max_cpus);
	if (!cpuset_possible)
		return;

	if (topo_path_read_cpulist(WAYCA_SC_CPU_FNAME, "possible",
				   cpuset_possible,
				   p_topo->kernel_max_cpus) == 0) {
		/* determine number of CPUs in cpuset_possible */
		p_topo->n_cpus = CPU_COUNT_S(p_topo->setsize, cpuset_possible);
		p_topo->cpu_map = cpuset_possible;
		/* allocate wayca_cpu for each CPU */
		p_topo->cpus = (struct wayca_cpu **)calloc(
			p_topo->n_cpus, sizeof(struct wayca_cpu *));
		if (!p_topo->cpus)
			goto cleanup_on_error;
	} else {
		PRINT_ERROR("failed to read possible CPUs");
		goto cleanup_on_error;
	}

	/* initialize node_map */
	p_topo->node_map = CPU_ALLOC(p_topo->n_cpus);
	if (!p_topo->node_map)
		goto cleanup_on_error;
	CPU_ZERO_S(CPU_ALLOC_SIZE(p_topo->n_cpus), p_topo->node_map);

	/* read all cpu%d topology */
	for (i = 0; i < p_topo->n_cpus; i++) {
		ret = topo_read_cpu_topology(i, p_topo);
		if (ret) {
			PRINT_ERROR("get cpu %d topology fail, ret = %d\n", i,
				    ret);
			goto cleanup_on_error;
		}
	}
	/* at the end of the cpu%d for loop, the following has been established:
	 *  - p_topo->n_nodes
	 *  - p_topo->node_map
	 *  - p_topo->nodes[]
	 *  - p_topo->n_clusters
	 *  - p_topo->ccls[]
	 *  - p_topo->n_packages
	 *  - p_topo->packages[]
	 */

	node_possible = CPU_ALLOC(p_topo->n_cpus);
	if (!node_possible)
		goto cleanup_on_error;

	/* read "node/possible" to determine number of numa nodes */
	if (topo_path_read_cpulist(WAYCA_SC_NODE_FNAME, "possible",
				   node_possible, p_topo->n_cpus) == 0) {
		if (!CPU_EQUAL_S(CPU_ALLOC_SIZE(p_topo->n_cpus), node_possible,
				 p_topo->node_map) ||
		    CPU_COUNT_S(CPU_ALLOC_SIZE(p_topo->n_cpus),
				node_possible) != p_topo->n_nodes) {
			/* mismatch with what cpu topology shows */
			PRINT_ERROR(
				"node/possible mismatch with what cpu topology shows");
			goto cleanup_on_error;
		}
	} else {
		PRINT_ERROR("failed to read possible NODEs");
		goto cleanup_on_error;
	}

	/* read all node%d topology */
	for (i = 0; i < p_topo->n_nodes; i++) {
		cpu_set_t *res_cpu;
		size_t setsize;
		int j;

		ret = topo_read_node_topology(i, p_topo);
		if (ret) {
			PRINT_ERROR("get node %d topology fail, ret = %d", i,
				    ret);
			goto cleanup_on_error;
		}
		res_cpu = CPU_ALLOC(p_topo->n_cpus);
		setsize = CPU_ALLOC_SIZE(p_topo->n_cpus);
		for (j = 0; j < p_topo->n_packages; j++) {
			CPU_AND_S(setsize, res_cpu,
				  p_topo->packages[j]->cpu_map,
				  p_topo->nodes[i]->cpu_map);
			if (CPU_EQUAL_S(setsize, res_cpu,
					p_topo->nodes[i]->cpu_map))
				CPU_SET_S(i, setsize,
					  p_topo->packages[j]->numa_map);

		}
	}

	/* at the end of the cpu%d for loop, the following has been established:
	 *  - p_topo->nodes[]->distance
	 */

	/* Construct wayca_cores topology from wayca_cpus */
	ret = topo_construct_core_topology(p_topo);
	if (ret) {
		PRINT_ERROR("Failed to construct core topology, ret = %d", ret);
		goto cleanup_on_error;
	}

	/* read I/O devices topology */
	if (topo_recursively_read_io_devices(WAYCA_SC_SYSDEV_FNAME, p_topo) !=
	    0)
		goto cleanup_on_error;

	chdir(origin_wd);
	return;

	/* cleanup_on_error */
cleanup_on_error:
	topo_free();
	return;
}

/* print the topology */
void topo_print_wayca_cluster(size_t setsize, struct wayca_cluster *p_cluster)
{
	PRINT_DBG("cluster_id: %08x\n", p_cluster->cluster_id);
	PRINT_DBG("n_cpus: %lu\n", p_cluster->n_cpus);
	PRINT_DBG("\tCpu count in this cluster's cpu_map: %d\n",
		  CPU_COUNT_S(setsize, p_cluster->cpu_map));
}

void topo_print_wayca_node(size_t setsize, struct wayca_node *p_node,
			size_t distance_size)
{
	int i, j;

	PRINT_DBG("node index: %d\n", p_node->node_idx);
	PRINT_DBG("n_cpus: %lu\n", p_node->n_cpus);
	PRINT_DBG("\tCpu count in this node's cpu_map: %d\n",
		  CPU_COUNT_S(setsize, p_node->cpu_map));
	PRINT_DBG("total memory (in kB): %8lu\n",
		  p_node->p_meminfo->total_avail_kB);
	PRINT_DBG("distance: ");
	for (i = 0; i < distance_size; i++)
		PRINT_DBG("%d\t", p_node->distance[i]);
	PRINT_DBG("\n");
	PRINT_DBG("n_pcidevs: %lu\n", p_node->n_pcidevs);
	for (i = 0; i < p_node->n_pcidevs; i++) {
		PRINT_DBG("\tpcidev%d: numa_node=%d\n", i,
			  p_node->pcidevs[i]->numa_node);
		PRINT_DBG("\t\t linked to SMMU No.: %d\n",
			  p_node->pcidevs[i]->smmu_idx);
		PRINT_DBG("\t\t enable(1) or not(0): %d\n",
			  p_node->pcidevs[i]->enable);
		PRINT_DBG("\t\t class=0x%06x\n", p_node->pcidevs[i]->class);
		PRINT_DBG("\t\t vendor=0x%04x\n", p_node->pcidevs[i]->vendor);
		PRINT_DBG("\t\t device=0x%04x\n", p_node->pcidevs[i]->device);
		PRINT_DBG("\t\t number of local CPUs: %d\n",
			  CPU_COUNT_S(setsize,
				      p_node->pcidevs[i]->local_cpu_map));
		PRINT_DBG("\t\t absolute_path: %s\n",
			  p_node->pcidevs[i]->absolute_path);
		PRINT_DBG("\t\t PCI_SLOT_NAME: %s\n",
			  p_node->pcidevs[i]->slot_name);
		PRINT_DBG("\t\t count of irqs (inc. msi_irqs): %d\n",
			  j = p_node->pcidevs[i]->irqs.n_irqs);
		PRINT_DBG("\t\t\t List of IRQs (irq_number:activeness:\"irq_name)\"\n");
		for (j = 0; j < p_node->pcidevs[i]->irqs.n_irqs; j++) {
			PRINT_DBG("\t\t\t\t %u:%u:\"%s\"\n",
				  p_node->pcidevs[i]->irqs.irqs[j].irq_number,
				  p_node->pcidevs[i]->irqs.irqs[j].active,
				  p_node->pcidevs[i]->irqs.irqs[j].irq_name);
		}
	}
	PRINT_DBG("n_smmus: %lu\n", p_node->n_smmus);
	for (int i = 0; i < p_node->n_smmus; i++) {
		PRINT_DBG("\tSMMU.%d:\n", p_node->smmus[i]->smmu_idx);
		PRINT_DBG("\t\t numa_node: %d\n", p_node->smmus[i]->numa_node);
		PRINT_DBG("\t\t base address : 0x%016llx\n",
			  p_node->smmus[i]->base_addr);
		PRINT_DBG("\t\t type(modalias): %s\n",
			  p_node->smmus[i]->modalias);
	}
	/* TODO: fill up the following */
	PRINT_DBG("pointer of cluster_map: 0x%p EXPECTED (nil)\n",
		  p_node->cluster_map);
}

void topo_print_wayca_cpu(size_t setsize, struct wayca_cpu *p_cpu)
{
	PRINT_DBG("cpu_id: %d\n", p_cpu->cpu_id);
	PRINT_DBG("core_id: %d\n", p_cpu->core_id);
	PRINT_DBG("\tCPU count in this core / SMT factor: %d\n",
		  CPU_COUNT_S(setsize, p_cpu->core_cpus_map));
	PRINT_DBG("Number of caches: %zu\n", p_cpu->n_caches);
	for (int i = 0; i < p_cpu->n_caches; i++) {
		PRINT_DBG("\tCache index %d:\n", i);
		PRINT_DBG("\t\tid: %d\n", p_cpu->p_caches[i].id);
		PRINT_DBG("\t\tlevel: %d\n", p_cpu->p_caches[i].level);
		PRINT_DBG("\t\ttype: %s\n", p_cpu->p_caches[i].type);
		PRINT_DBG("\t\tallocation_policy: %s\n",
			  p_cpu->p_caches[i].allocation_policy);
		PRINT_DBG("\t\twrite_policy: %s\n",
			  p_cpu->p_caches[i].write_policy);
		PRINT_DBG("\t\tcache_size: %s\n",
			  p_cpu->p_caches[i].cache_size);
		PRINT_DBG("\t\tways_of_associativity: %u\n",
			  p_cpu->p_caches[i].ways_of_associativity);
		PRINT_DBG("\t\tphysical_line_partition: %u\n",
			  p_cpu->p_caches[i].physical_line_partition);
		PRINT_DBG("\t\tnumber_of_sets: %u\n",
			  p_cpu->p_caches[i].number_of_sets);
		PRINT_DBG("\t\tcoherency_line_size: %u\n",
			  p_cpu->p_caches[i].coherency_line_size);
		PRINT_DBG("\t\tshared with how many cores: %d\n",
			  CPU_COUNT_S(setsize,
				      p_cpu->p_caches[i].shared_cpu_map));
	}
	if (p_cpu->p_cluster != NULL) {
		PRINT_DBG("belongs to cluster_id: \t%08x\n",
			  p_cpu->p_cluster->cluster_id);
	}
	PRINT_DBG("belongs to node: \t%d\n", p_cpu->p_numa_node->node_idx);
	PRINT_DBG("belongs to package_id: \t%08x\n",
		  p_cpu->p_package->physical_package_id);
}

void topo_print_wayca_core(size_t setsize, struct wayca_core *p_core)
{
	PRINT_DBG("core_id: %d\n", p_core->core_id);
	PRINT_DBG("\tn_cpus: %lu\n", p_core->n_cpus);
	PRINT_DBG("\tCPU count in this core / SMT factor: %d\n",
		  CPU_COUNT_S(setsize, p_core->core_cpus_map));
	PRINT_DBG("Number of caches: %zu\n", p_core->n_caches);
	if (p_core->p_cluster != NULL)
		PRINT_DBG("belongs to cluster_id: \t%08x\n",
			  p_core->p_cluster->cluster_id);
	PRINT_DBG("belongs to node: \t%d\n", p_core->p_numa_node->node_idx);
	WAYCA_SC_LOG_INFO("belongs to package_id: \t%08x\n",
			  p_core->p_package->physical_package_id);
	return;
}

#ifdef WAYCA_SC_DEBUG
void wayca_sc_topo_print(void)
{
	struct wayca_topo *p_topo = &topo;
	int i;

	PRINT_DBG("kernel_max_cpus: %d\n", p_topo->kernel_max_cpus);
	PRINT_DBG("setsize: %lu\n", p_topo->setsize);

	PRINT_DBG("n_cpus: %lu\n", p_topo->n_cpus);
	PRINT_DBG("\tCpu count in cpu_map: %d\n",
		  CPU_COUNT_S(p_topo->setsize, p_topo->cpu_map));
	for (i = 0; i < p_topo->n_cpus; i++) {
		if (p_topo->cpus[i] == NULL)
			continue;
		PRINT_DBG("CPU%d information:\n", i);
		topo_print_wayca_cpu(p_topo->setsize, p_topo->cpus[i]);
	}

	PRINT_DBG("n_cores: %lu\n", p_topo->n_cores);
	for (i = 0; i < p_topo->n_cores; i++) {
		if (p_topo->cores[i] == NULL)
			continue;
		PRINT_DBG("Core %d information:\n", i);
		topo_print_wayca_core(p_topo->setsize, p_topo->cores[i]);
	}

	PRINT_DBG("n_clusters: %lu\n", p_topo->n_clusters);
	for (i = 0; i < p_topo->n_clusters; i++) {
		if (p_topo->ccls[i] == NULL)
			continue;
		PRINT_DBG("Cluster %d information:\n", i);
		topo_print_wayca_cluster(p_topo->setsize, p_topo->ccls[i]);
	}

	PRINT_DBG("n_nodes: %lu\n", p_topo->n_nodes);
	PRINT_DBG("\tNode count in node_map: %d\n",
		  CPU_COUNT_S(CPU_ALLOC_SIZE(p_topo->n_cpus),
			      p_topo->node_map));
	for (i = 0; i < p_topo->n_nodes; i++) {
		if (p_topo->nodes[i] == NULL)
			continue;
		PRINT_DBG("NODE%d information:\n", i);
		topo_print_wayca_node(p_topo->setsize, p_topo->nodes[i],
				      p_topo->n_nodes);
	}
	PRINT_DBG("n_packages: %lu\n", p_topo->n_packages);

	return;
}
#endif /* WAYCA_SC_DEBUG */

/* topo_free - free up memories */
void topo_free(void)
{
	struct wayca_topo *p_topo = &topo;
	int i, j;

	CPU_FREE(p_topo->cpu_map);
	for (i = 0; i < p_topo->n_cpus; i++) {
		CPU_FREE(p_topo->cpus[i]->core_cpus_map);
		free(p_topo->cpus[i]->p_caches);
		free(p_topo->cpus[i]);
	}
	free(p_topo->cpus);

	/* NOTE: pointers inside wayca_core are freed by wayca_cpu
	 * So, here we only need to free the top-level wayca_core structure
	 */
	for (i = 0; i < p_topo->n_cores; i++)
		free(p_topo->cores[i]);
	free(p_topo->cores);

	for (i = 0; i < p_topo->n_clusters; i++)
		CPU_FREE(p_topo->ccls[i]->cpu_map);
	free(p_topo->ccls);

	CPU_FREE(p_topo->node_map);
	for (i = 0; i < p_topo->n_nodes; i++) {
		CPU_FREE(p_topo->nodes[i]->cpu_map);
		CPU_FREE(p_topo->nodes[i]->cluster_map);
		free(p_topo->nodes[i]->distance);
		free(p_topo->nodes[i]->p_meminfo);
		for (j = 0; j < p_topo->nodes[i]->n_pcidevs; j++) {
			CPU_FREE(p_topo->nodes[i]->pcidevs[j]->local_cpu_map);
			free(p_topo->nodes[i]->pcidevs[j]->irqs.irqs);
			free(p_topo->nodes[i]->pcidevs[j]);
		}
		free(p_topo->nodes[i]->pcidevs);
		for (j = 0; j < p_topo->nodes[i]->n_smmus; j++)
			free(p_topo->nodes[i]->smmus[j]);
		free(p_topo->nodes[i]->smmus);
		free(p_topo->nodes[i]);
	}
	free(p_topo->nodes);

	for (i = 0; i < p_topo->n_packages; i++) {
		CPU_FREE(p_topo->packages[i]->cpu_map);
		CPU_FREE(p_topo->packages[i]->numa_map);
		free(p_topo->packages[i]);
	}
	free(p_topo->packages);

	memset(p_topo, 0, sizeof(struct wayca_topo));
	return;
}

int wayca_sc_cpus_in_core(void)
{
	if (topo.n_cores < 1)
		return -ENODATA; /* not initialized */
	return topo.cores[0]->n_cpus;
}

int wayca_sc_cpus_in_ccl(void)
{
	if (topo.n_clusters < 1)
		return -ENODATA; /* not initialized */
	return topo.ccls[0]->n_cpus;
}

int wayca_sc_cpus_in_node(void)
{
	if (topo.n_nodes < 1)
		return -ENODATA; /* not initialized */
	return topo.nodes[0]->n_cpus;
}

int wayca_sc_cpus_in_package(void)
{
	if (topo.n_packages < 1)
		return -ENODATA; /* not initialized */
	return topo.packages[0]->n_cpus;
}

int wayca_sc_cpus_in_total(void)
{
	if (topo.n_cpus < 1)
		return -ENODATA; /* not initialized */
	return topo.n_cpus;
}

int wayca_sc_cores_in_ccl(void)
{
	if (topo.n_clusters < 1)
		return -ENODATA; /* not initialized */
	if (topo.n_cores < 1)
		return -ENODATA; /* not initialized */
	return topo.ccls[0]->n_cpus / topo.cores[0]->n_cpus;
}

int wayca_sc_cores_in_node(void)
{
	if (topo.n_cores < 1)
		return -ENODATA; /* not initialized */
	return topo.nodes[0]->n_cpus / topo.cores[0]->n_cpus;
}

int wayca_sc_cores_in_package(void)
{
	if (topo.n_cores < 1)
		return -ENODATA; /* not initialized */
	return topo.packages[0]->n_cpus / topo.cores[0]->n_cpus;
}

int wayca_sc_cores_in_total(void)
{
	if (topo.n_cores < 1)
		return -ENODATA; /* not initialized */
	return topo.n_cores;
}

int wayca_sc_ccls_in_package(void)
{
	if (topo.n_clusters < 1)
		return -ENODATA; /* not initialized */
	return topo.packages[0]->n_cpus / topo.ccls[0]->n_cpus;
}

int wayca_sc_ccls_in_node(void)
{
	if (topo.n_clusters < 1)
		return -ENODATA; /* not initialized */
	return topo.nodes[0]->n_cpus / topo.ccls[0]->n_cpus;
}

int wayca_sc_ccls_in_total(void)
{
	if (topo.n_clusters < 1)
		return -ENODATA; /* not initialized */
	return topo.n_clusters;
}

int wayca_sc_nodes_in_package(void)
{
	if (topo.n_packages < 1)
		return -ENODATA; /* not initialized */
	return topo.packages[0]->n_cpus / topo.nodes[0]->n_cpus;
}

int wayca_sc_nodes_in_total(void)
{
	if (topo.n_nodes < 1)
		return -ENODATA; /* not initialized */
	return topo.n_nodes;
}

int wayca_sc_packages_in_total(void)
{
	if (topo.n_packages < 1)
		return -ENODATA; /* not initialized */
	return topo.n_packages;
}

static bool topo_is_valid_cpu(int cpu_id)
{
	return cpu_id >= 0 && cpu_id < wayca_sc_cores_in_total();
}

static bool topo_is_valid_core(int core_id)
{
	return core_id >= 0 && core_id < wayca_sc_cpus_in_total();
}

static bool topo_is_valid_ccl(int ccl_id)
{
	return ccl_id >= 0 && ccl_id < wayca_sc_ccls_in_total();
}

static bool topo_is_valid_node(int node_id)
{
	return node_id >= 0 && node_id < wayca_sc_nodes_in_total();
}

static bool topo_is_valid_package(int package_id)
{
	return package_id >= 0 && package_id < wayca_sc_packages_in_total();
}

int wayca_sc_core_cpu_mask(int core_id, size_t cpusetsize, cpu_set_t *mask)
{
	size_t valid_cpu_setsize;

	if (mask == NULL || !topo_is_valid_core(core_id))
		return -EINVAL;

	valid_cpu_setsize = CPU_ALLOC_SIZE(topo.n_cpus);
	if (cpusetsize < valid_cpu_setsize)
		return -EINVAL;

	CPU_ZERO_S(cpusetsize, mask);
	CPU_OR_S(valid_cpu_setsize, mask, mask,
		 topo.cores[core_id]->core_cpus_map);
	return 0;
}

int wayca_sc_ccl_cpu_mask(int ccl_id, size_t cpusetsize, cpu_set_t *mask)
{
	size_t valid_cpu_setsize;

	if (mask == NULL || !topo_is_valid_ccl(ccl_id))
		return -EINVAL;

	valid_cpu_setsize = CPU_ALLOC_SIZE(topo.n_cpus);
	if (cpusetsize < valid_cpu_setsize)
		return -EINVAL;

	CPU_ZERO_S(cpusetsize, mask);
	CPU_OR_S(valid_cpu_setsize, mask, mask, topo.ccls[ccl_id]->cpu_map);
	return 0;
}

int wayca_sc_node_cpu_mask(int node_id, size_t cpusetsize, cpu_set_t *mask)
{
	size_t valid_cpu_setsize;

	if (mask == NULL || !topo_is_valid_node(node_id))
		return -EINVAL;

	valid_cpu_setsize = CPU_ALLOC_SIZE(topo.n_cpus);
	if (cpusetsize < valid_cpu_setsize)
		return -EINVAL;

	CPU_ZERO_S(cpusetsize, mask);
	CPU_OR_S(valid_cpu_setsize, mask, mask, topo.nodes[node_id]->cpu_map);
	return 0;
}

int wayca_sc_package_cpu_mask(int package_id, size_t cpusetsize,
			      cpu_set_t *mask)
{
	size_t valid_cpu_setsize;

	if (mask == NULL || !topo_is_valid_package(package_id))
		return -EINVAL;

	valid_cpu_setsize = CPU_ALLOC_SIZE(topo.n_cpus);
	if (cpusetsize < valid_cpu_setsize)
		return -EINVAL;

	CPU_ZERO_S(cpusetsize, mask);
	CPU_OR_S(valid_cpu_setsize, mask, mask,
		 topo.packages[package_id]->cpu_map);
	return 0;
}

int wayca_sc_total_cpu_mask(size_t cpusetsize, cpu_set_t *mask)
{
	size_t valid_cpu_setsize;

	if (mask == NULL)
		return -EINVAL;

	valid_cpu_setsize = CPU_ALLOC_SIZE(topo.n_cpus);
	if (cpusetsize < valid_cpu_setsize)
		return -EINVAL;

	CPU_ZERO_S(cpusetsize, mask);
	CPU_OR_S(valid_cpu_setsize, mask, mask, topo.cpu_map);
	return 0;
}

int wayca_sc_package_node_mask(int package_id, size_t setsize, cpu_set_t *mask)
{
	size_t valid_numa_setsize;

	if (mask == NULL || !topo_is_valid_package(package_id))
		return -EINVAL;

	valid_numa_setsize = CPU_ALLOC_SIZE(topo.n_nodes);
	if (setsize < valid_numa_setsize)
		return -EINVAL;

	CPU_ZERO_S(setsize, mask);
	CPU_OR_S(valid_numa_setsize, mask, mask,
		 topo.packages[package_id]->numa_map);
	return 0;
}

int wayca_sc_total_node_mask(size_t setsize, cpu_set_t *mask)
{
	size_t valid_numa_setsize;

	if (mask == NULL)
		return -EINVAL;

	valid_numa_setsize = CPU_ALLOC_SIZE(topo.n_nodes);
	if (setsize < valid_numa_setsize)
		return -EINVAL;

	CPU_ZERO_S(setsize, mask);
	CPU_OR_S(valid_numa_setsize, mask, mask, topo.node_map);
	return 0;
}

int wayca_sc_get_core_id(int cpu_id)
{
	if (!topo_is_valid_cpu(cpu_id))
		return -EINVAL;

	return topo.cpus[cpu_id]->core_id;
}

int wayca_sc_get_ccl_id(int cpu_id)
{
	int physical_id;
	int i;

	// cluster may not exist in some version of kernel
	if (!topo_is_valid_cpu(cpu_id) || wayca_sc_cpus_in_ccl() < 0)
		return -EINVAL;

	physical_id = topo.cpus[cpu_id]->p_cluster->cluster_id;
	for (i = 0; i < topo.n_clusters; i++) {
		if (topo.ccls[i]->cluster_id == physical_id)
			return i;
	}
	return -EINVAL;
}

int wayca_sc_get_node_id(int cpu_id)
{
	if (!topo_is_valid_cpu(cpu_id))
		return -EINVAL;

	return topo.cpus[cpu_id]->p_numa_node->node_idx;
}

int wayca_sc_get_package_id(int cpu_id)
{
	int physical_id;
	int i;

	if (!topo_is_valid_cpu(cpu_id))
		return -EINVAL;

	physical_id = topo.cpus[cpu_id]->p_package->physical_package_id;
	for (i = 0; i < topo.n_packages; i++) {
		if (topo.packages[i]->physical_package_id == physical_id)
			return i;
	}
	return -EINVAL;
}

int wayca_sc_get_node_mem_size(int node_id, unsigned long *size)
{
	if (size == NULL || !topo_is_valid_node(node_id))
		return -EINVAL;

	*size = topo.nodes[node_id]->p_meminfo->total_avail_kB;
	return 0;
}

static int parse_cache_size(const char *size)
{
	int cache_size;
	char *endstr;

	cache_size = strtol(size, &endstr, 10);
	if (cache_size < 0 || *endstr != 'K')
		return -EINVAL;

	return cache_size;
}

int wayca_sc_get_l1i_size(int cpu_id)
{
	static const char *size;
	static const char *type;
	int level;
	int i;

	if (!topo_is_valid_cpu(cpu_id))
		return -EINVAL;

	for (i = 0; i < topo.cpus[cpu_id]->n_caches; i++) {
		level = topo.cpus[cpu_id]->p_caches[i].level;
		type = topo.cpus[cpu_id]->p_caches[i].type;
		if (level == 1 && !strcmp(type, "Instruction")) {
			size = topo.cpus[cpu_id]->p_caches[i].cache_size;
			return parse_cache_size(size);
		}
	}
	return -ENODATA;
}

int wayca_sc_get_l1d_size(int cpu_id)
{
	static const char *size;
	static const char *type;
	int level;
	int i;

	if (!topo_is_valid_cpu(cpu_id))
		return -EINVAL;

	for (i = 0; i < topo.cpus[cpu_id]->n_caches; i++) {
		level = topo.cpus[cpu_id]->p_caches[i].level;
		type = topo.cpus[cpu_id]->p_caches[i].type;
		if (level == 1 && !strcmp(type, "Data")) {
			size = topo.cpus[cpu_id]->p_caches[i].cache_size;
			return parse_cache_size(size);
		}
	}
	return -ENODATA;
}

int wayca_sc_get_l2_size(int cpu_id)
{
	static const char *size;
	static const char *type;
	int level;
	int i;

	if (!topo_is_valid_cpu(cpu_id))
		return -EINVAL;

	for (i = 0; i < topo.cpus[cpu_id]->n_caches; i++) {
		level = topo.cpus[cpu_id]->p_caches[i].level;
		type = topo.cpus[cpu_id]->p_caches[i].type;
		if (level == 2 && !strcmp(type, "Unified")) {
			size = topo.cpus[cpu_id]->p_caches[i].cache_size;
			return parse_cache_size(size);
		}
	}
	return -ENODATA;
}

int wayca_sc_get_l3_size(int cpu_id)
{
	static const char *size;
	static const char *type;
	int level;
	int i;

	if (!topo_is_valid_cpu(cpu_id))
		return -EINVAL;

	for (i = 0; i < topo.cpus[cpu_id]->n_caches; i++) {
		level = topo.cpus[cpu_id]->p_caches[i].level;
		type = topo.cpus[cpu_id]->p_caches[i].type;
		if (level == 3 && !strcmp(type, "Unified")) {
			size = topo.cpus[cpu_id]->p_caches[i].cache_size;
			return parse_cache_size(size);
		}
	}
	return -ENODATA;
}

/* wayca_sc_get_pcidev_irqs - given a PCI/e device name 'dev_name',
 *			      return irqs' info
 * Note: This routine malloc' spaces for p_irqs and irq_names on success. It
 *       is the call's responsibility to free them.
 * Input:
 *   - dev_name: device's name. For a PCI device, it is PCI_SLOT_NAME.
 * Output:
 *   - n_irqs: number of irqs for this device
 *   - p_irqs: pointer to interrupt numbers array
 *   - irq_names: interrupt names array. Each irq_name is a string of
 *		  length WAYCA_SC_ATTR_STRING_LEN. Note:
 *		(only active irqs have names, as in /proc/interrupts)
 *		(inactive irqs were not listed in /proc/interrupts,
 *		  so have no names. In that case, it's filled by NULL)
 * Return: negative on error, 0 on success
 *	-ENODEV, the named pcidev doesn't exist
 *	-ENOMEM, no enough memory
 */
int wayca_sc_get_pcidev_irqs(const char *dev_name, size_t *n_irqs,
			     unsigned int **p_irqs, char **irq_names)
{
	int i, j;
	bool found = false;
	struct wayca_device_irqs *p_dev_irqs = NULL;
	char (*p_irq_name)[WAYCA_SC_ATTR_STRING_LEN];

	/* search for dev_name */
	for (i = 0; i < topo.n_nodes; i++) {
		for (j = 0; j < topo.nodes[i]->n_pcidevs; j++) {
			/* compare dev_name with slot_name */
			if (strncmp(dev_name,
				   topo.nodes[i]->pcidevs[j]->slot_name,
				   WAYCA_SC_NAME_LEN_MAX) == 0) {
				found = true;
				p_dev_irqs = &topo.nodes[i]->pcidevs[j]->irqs;
				break;
			}
		}
		if (found == true) break;
	}

	/* return early if not found */
	if (found == false) {
		*n_irqs = 0;
		*p_irqs = NULL;
		return -ENODEV;
	}

	/* output */
	*n_irqs = p_dev_irqs->n_irqs;
	/* set interrupt numbers arrary */
	*p_irqs = (unsigned int *)calloc(1, sizeof(unsigned int) * (*n_irqs));
	if (p_irqs == NULL)
		return -ENOMEM;		/* no enough memory */

	for (i = 0; i < (*n_irqs); i++) {
		(*p_irqs)[i] = p_dev_irqs->irqs[i].irq_number;
	}
	/* set interrupt names arrary. Each irq_name is a string of
	 * length WAYCA_SC_ATTR_STRING_LEN */
	p_irq_name = (char (*)[WAYCA_SC_ATTR_STRING_LEN])calloc(*n_irqs,
				sizeof(char) * WAYCA_SC_ATTR_STRING_LEN);
	if (p_irq_name == NULL) {
		free(*p_irqs);
		*p_irqs = NULL;
		return -ENOMEM;		/* no enough memory */
	}

	for (i = 0; i < (*n_irqs); i++)
		strcpy(p_irq_name[i], p_dev_irqs->irqs[i].irq_name);
	*irq_names = (char *)p_irq_name;

	return 0;
}

/* memory bandwidth (relative value) of speading over multiple CCLs
 *
 * Measured with: bw_mem bcopy
 *
 * Implication:
 *  6 CCLs (clusters) per NUMA node
 *  multiple threads run spreadingly over multiple Clusters. One thread per core.
 */
int mem_bandwidth_6CCL[][6] = {
	/* 1CCL, 2CCLs, 3CCLs, 4CCLs, 5CCLs, 6CCLs */
	{ 15, 18, 18, 19, 19, 19 }, /* 4 threads */
	{ 0, 23, 23, 24, 24, 24 }, /* 8 threads */
	{ 0, 0, 28, 28, 28, 29 }, /* 12 threads */
	{ 0, 0, 0, 31, 32, 31 }, /* 16 threads */
};

/* memory bandwidth (relative value) of speading over multiple NUMA nodes
 *
 * Measured with: bw_mem bcopy
 *
 * Implication:
 *  4 NUMA nodes
 *  multiple threads run spreadingly over multiple NUMA nodes. One thread per core.
 */
int mem_bandwidth_4NUMA[][4] = {
	/* 1NUMA, 2NUMA, 3NUMA, 4NUMA */
	{ 33, 55, 68, 79 }, /* 24 threads */
	{ 0, 66, 92, 112 }, /* 48 threads */
	{ 0, 0, 99, 130 } /* 72 threads */
};

/* memory bandwidth when computing is on one NUMA, but memory is interleaved on different NUMA node(s)
 * Measured with: numactl --interleave, bw_mem bcopy
 *
 * Implication:
 *  4 NUMA nodes
 *  multiple threads run spreadingly over multiple NUMA nodes. One thread per core.
 */
int mem_bandwidth_interleave_4NUMA[][7] = {
	/* Same | Neighbor | Remote | Remote | Neighbor |         |         *
	 * NUMA |  NUMA    | NUMA0  | NUMA1  |  2 NUMAs | 3 NUMAs | 4 NUMAs */
	{ 19, 5, 9, 6, 9, 11, 9 }, /* 4 threads */
	{ 24, 5, 7, 6, 10, 14, 13 }, /* 8 threads */
	{ 29, 5, 7, 6, 10, 15, 13 }, /* 12 threads */
	{ 31, 5, 7, 6, 10, 16, 13 } /* 16 threads */
};

/* memory read latency for range [1M ~ 8M], multiple threads spreading over multiple CCLs, same NUMA
 *
 * Measured with: lat_mem_rd
 * Implication:
 *  6 CCLs (clusters) per NUMA node
 *  multiple threads run spreadingly over multiple Clusters. One thread per core.
 */
int mem_rd_latency_1M_6CCL[][6] = {
	/* 1CCL, 2CCLs, 3CCLs, 4CCLs, 5CCLs, 6CCLs */
	{ 13, 6, 4, 4, 4, 4 }, /* 4 threads */
	{ 0, 12, 6, 9, 5, 5 }, /* 8 threads */
	{ 0, 0, 16, 15, 12, 10 }, /* 12 threads */
	{ 0, 0, 0, 17, 14, 12 }, /* 16 threads */
};

/* memory read latency for range [12M ~ 2G+], multiple threads spreading over multiple CCLs, same NUMA
 *
 * Measured with: lat_mem_rd
 * Implication:
 *  6 CCLs (clusters) per NUMA node
 *  multiple threads run spreadingly over multiple Clusters. One thread per core.
 */
int mem_rd_latency_12M_6CCL[][6] = {
	/* 1CCL, 2CCLs, 3CCLs, 4CCLs, 5CCLs, 6CCLs */
	{ 13, 8, 6, 6, 6, 6 }, /* 4 threads */
	{ 0, 14, 9, 9, 8, 8 }, /* 8 threads */
	{ 0, 0, 15, 12, 11, 11 }, /* 12 threads */
	{ 0, 0, 0, 16, 14, 13 }, /* 16 threads */
};

/* memory read latency for range [1M ~ 8M], multiple threads spreading over multiple NUMAs
 *
 * Measured with: lat_mem_rd
 * Implication:
 *  4 NUMA nodes
 *  multiple threads run spreadingly over multiple NUMAs. One thread per core.
 */
int mem_rd_latency_1M_4NUMA[][4] = {
	/* 1NUMA, 2NUMA, 3NUMA, 4NUMA */
	{ 19, 16, 11, 6 }, /* 24 threads */
	{ 0, 19, 17, 14 }, /* 48 threads */
	{ 0, 0, 17, 9 } /* 72 threads */
};

/* memory read latency for range [12M ~ 2G+], multiple threads spreading over multiple NUMAs
 *
 * Measured with: lat_mem_rd
 * Implication:
 *  4 NUMA nodes
 *  multiple threads run spreadingly over multiple NUMAs. One thread per core.
 */
int mem_rd_latency_12M_4NUMA[][4] = {
	/* 1NUMA, 2NUMA, 3NUMA, 4NUMA */
	{ 21, 15, 14, 8 }, /* 24 threads */
	{ 0, 20, 16, 15 }, /* 48 threads */
	{ 0, 0, 18, 12 } /* 72 threads */
};

/* pipe latency within the same CCL, and across two CCLs of the same NUMA
 *
 * Measaured with: lat_pipe
 * Implications:
 *  6 CCLs (clusters) per NUMA node
 *  Pipe latencies between different CCLs have no notice-worthy difference. Just categorized
 *     'cross CCLs' in below.
 */
int pipe_latency_CCL[3] = {
	/* same | same | cross *
	 * CPU  | CCL  | CCLs  */
	46, 49, 66 /* 2 processes in pipe communitcaiton */
};

/* pipe latency across NUMA nodes */
int pipe_latency_NUMA[4] = {
	/* Same NUMA | Neighbor | Remote | Remote *
	 * diff CCLs |  NUMAs   | NUMA0  | NUMA1  */
};

/* topo_query_proc_interrupts - find whether an irq is active in /proc/interrupts
 *   and get the irq_name
 * Input:
 *   - irq: the irq number to query
 * Output:
 *   - active: whether exists in /proc/interrupts
 *   - irq_name: this irq's name, maximum length WAYCA_SC_ATTR_STRING_LEN
 * Return: negative on error, 0 on success.
 */
static int topo_query_proc_interrupts(unsigned int irq, unsigned int *active,
				      char *irq_name)
{
	unsigned int this_irq;
	char *line = NULL;
	size_t sz = 0;
	int ret = 0;
	char *c;
	FILE *f;

	/* initialize to inactive, unless we found differently later */
	*active = 0;

	f = fopen("/proc/interrupts", "r");
	if (f == NULL)
		return -errno;

	/* read the first line, but ignore it since it's the header */
	if (getline(&line, &sz, f) == -1) {
		free(line);
		fclose(f);
		return -errno;
	}

	/* search the second line and all after */
	while (!feof(f)) {

		/* read one line */
		if (getline(&line, &sz, f) == -1) {
			ret = -errno;
			break;
		}
		c = line;
		/* skip the leading spaces */
		while (isblank(*c))
			c++;
		/* a useful line should start with a series of number, then ':' */
		if (!isdigit(*c))
			continue;
		c = strchr(line, ':');
		if (c == NULL)
			continue;

		/* convert it to a number */
		this_irq = (unsigned int)strtoul(line, NULL, 10);

		if (this_irq != irq)
			continue;	/* check next line */

		/* set active */
		*active = 1;
		/* move along, stop at the first non-digit character
		 * , which will be the name part of the irq
		 */
		c++;
		while (isblank(*c) || isdigit(*c))
			c++;

		strncpy(irq_name, c, WAYCA_SC_ATTR_STRING_LEN);
		sz = strlen(irq_name);
		if (irq_name[sz - 1] == '\n')	/* remove the ending newline */
			irq_name[sz - 1] = '\0';

		irq_name[WAYCA_SC_ATTR_STRING_LEN -1] = '\0';	/* in case of truncated string */
		break;
	}

	/* exit */
	fclose(f);
	free(line);
	return ret;
}

/* parse I/O device irqs information
 * Input: device_sysfs_dir, this device's absolute pathname in /sys/devices
 * Return negative on error, 0 on success
 */
static int topo_parse_device_irqs(const char *device_sysfs_dir,
				  struct wayca_device_irqs *wirqs)
{
	DIR *dp;
	struct dirent *entry;
	struct stat statbuf;
	int msi_irqs_exist = 0;
	int irq_file_exist = 0;
	int msi_irqs_count = 0;
	char path_buffer[WAYCA_SC_PATH_LEN_MAX];
	struct wayca_irq *p_irqs;

	wirqs->n_irqs = 0;

	/* find "msi_irqs" and/or "irq" */
	dp = opendir(device_sysfs_dir);
	if (!dp)
		return -errno;
	while (((msi_irqs_exist == 0) || (irq_file_exist == 0)) &&
		(entry = readdir(dp)) != NULL) {
		lstat(entry->d_name, &statbuf);
		if ((msi_irqs_exist == 0) && S_ISDIR(statbuf.st_mode)) {
			if (strcmp("msi_irqs", entry->d_name) == 0) {
				msi_irqs_exist = 1;
				PRINT_DBG("FOUND msi_irqs directory under %s\n",
					  device_sysfs_dir);
				continue;
			}
		} else if ((irq_file_exist == 0) &&
			   (strcmp("irq", entry->d_name) == 0)) {
			irq_file_exist = 1;
			PRINT_DBG("FOUND irq file under %s\n",
				  device_sysfs_dir);
			continue;
		}
	}
	closedir(dp);

	if (msi_irqs_exist == 1) {
		int i = 0;

		sprintf(path_buffer, "%s/msi_irqs", device_sysfs_dir);

		dp = opendir(path_buffer);
		if (dp == NULL)
			return -errno;
		while ((entry = readdir(dp)) != NULL) {
			/* If the entry is a regular file */
			if (entry->d_type == DT_REG)
				msi_irqs_count++;
		}
		wirqs->n_irqs = msi_irqs_count;
		PRINT_DBG("Found %d interrupts in msi_irqs\n", msi_irqs_count);

		/* allocate irq structs */
		p_irqs = (struct wayca_irq *)calloc(msi_irqs_count,
						    sizeof(struct wayca_irq));
		if (!p_irqs) {
			closedir(dp);
			return -ENOMEM;
		}
		wirqs->irqs = p_irqs;

		/* fill in irq_number */
		rewinddir(dp);

		PRINT_DBG("\tInterrupt numbers: ");
		while ((msi_irqs_count != 0) && (entry = readdir(dp)) != NULL) {
			/* If the entry is a regular file */
			if (entry->d_type == DT_REG) {
				p_irqs[i++].irq_number = atoi(entry->d_name);
				PRINT_DBG("%u\t", p_irqs[i - 1].irq_number);
				msi_irqs_count--;
				/* get active status and irq_name */
				topo_query_proc_interrupts(
					p_irqs[i - 1].irq_number,
					&p_irqs[i - 1].active,
					p_irqs[i - 1].irq_name);
			}
		}
		PRINT_DBG("\n");
		closedir(dp);
	}

	if (irq_file_exist == 1) {
		int irq_number = 0;
		int j;

		topo_path_read_s32(device_sysfs_dir, "irq", &irq_number);
		PRINT_DBG("irq file exists, irq number is: %d\n", irq_number);
		if (irq_number < 0)
			irq_number = 0;	/* on failure, default to set 0 */
		/* check it exists in wirqs or not */
		for (j = 0; j < wirqs->n_irqs; j++)
			if (wirqs->irqs[j].irq_number == irq_number)
				break;

		/* doesn't exist, create a new one */
		if (j == wirqs->n_irqs) {
			p_irqs = (struct wayca_irq *)realloc(
				wirqs->irqs,
				(wirqs->n_irqs + 1) * sizeof(struct wayca_irq));
			if (!p_irqs)
				return -ENOMEM;
			p_irqs[wirqs->n_irqs].irq_number = irq_number;
			p_irqs[wirqs->n_irqs].active = 0;
			p_irqs[wirqs->n_irqs].irq_name[0] = '\0';
			/* get active status and irq_name */
			topo_query_proc_interrupts(p_irqs[wirqs->n_irqs].irq_number,
				&p_irqs[wirqs->n_irqs].active,
				p_irqs[wirqs->n_irqs].irq_name);
			wirqs->n_irqs++;
			wirqs->irqs = p_irqs;
		}
	}

	return 0;
}

/* Return negative on error, 0 on success
 */
static int topo_parse_io_device(const char *dir, struct wayca_topo *p_topo)
{
	DIR *dp;
	struct dirent *entry;

	int node_nb;
	int i;
	int ret;
	char *p_index;
	char buf[WAYCA_SC_ATTR_STRING_LEN];
	char path_buffer[WAYCA_SC_PATH_LEN_MAX];
	struct wayca_pci_device *p_pcidev;
	struct wayca_smmu *p_smmu;

	if (strstr(dir, "pci")) {
		PRINT_DBG("PCI full path: %s\n", dir);
		/* allocate struct wayca_pci_device */
		p_pcidev = (struct wayca_pci_device *)calloc(
			1, sizeof(struct wayca_pci_device));
		if (!p_pcidev)
			return -ENOMEM;

		/* store dir full path */
		strcpy(p_pcidev->absolute_path, dir);
		PRINT_DBG("absolute path: %s\n", p_pcidev->absolute_path);

		/* cut out PCI_SLOT_NAME from the absolute path, which is last part of the path */
		if ((p_index = rindex(dir, '/')) != NULL) {
			strncpy(p_pcidev->slot_name, p_index + 1, WAYCA_SC_NAME_LEN_MAX - 1);
			PRINT_DBG("slot_name : %s\n", p_pcidev->slot_name);
		}

		/* read 'numa_node' */
		topo_path_read_s32(dir, "numa_node", &node_nb);
		PRINT_DBG("numa_node: %d\n", node_nb);
		if (node_nb < 0)
			node_nb = 0; /* on failure, default to node #0 */
		p_pcidev->numa_node = node_nb;

		/* get the 'wayca_node *' whoes node_idx == this 'node_nb' */
		for (i = 0; i < p_topo->n_nodes; i++) {
			if (p_topo->nodes[i]->node_idx == node_nb)
				break;
		}
		if (i == p_topo->n_nodes) {
			PRINT_ERROR(
				"failed to match this PCI device to any numa node: %s",
				dir);
			free(p_pcidev);
			return -1;
		}
		/* append p_pcidev to 'wayca node *'->pcidevs */
		struct wayca_pci_device **p_temp;

		p_temp = (struct wayca_pci_device **)realloc(
			p_topo->nodes[i]->pcidevs,
			(p_topo->nodes[i]->n_pcidevs + 1) *
				sizeof(struct wayca_pci_device *));
		if (!p_temp) {
			free(p_pcidev);
			return -ENOMEM;
		}
		p_topo->nodes[i]->pcidevs = p_temp;
		p_topo->nodes[i]->pcidevs[p_topo->nodes[i]->n_pcidevs] =
			p_pcidev;
		p_topo->nodes[i]
			->n_pcidevs++; /* incement number of PCI devices */
		PRINT_DBG("n_pcidevs = %zu\n", p_topo->nodes[i]->n_pcidevs);

		/*
		 * read PCI information into: p_pcidev.
		 * Sysfs data output format is defined in linux kernel code:
		 * [linux.kernel]/drivers/pci/pci-sysfs.c
		 * The format is class:vendor:device
		 */
		ret = topo_path_read_buffer(dir, "class", buf,
					    WAYCA_SC_ATTR_STRING_LEN);
		if (ret <= 0)
			p_pcidev->class = 0;
		else {
			if (sscanf(buf, "%x", &p_pcidev->class) != 1)
				p_pcidev->class = 0;
			PRINT_DBG("class: 0x%06x\n", p_pcidev->class);
		}
		ret = topo_path_read_buffer(dir, "vendor", buf,
					    WAYCA_SC_ATTR_STRING_LEN);
		if (ret <= 0)
			p_pcidev->vendor = 0;
		else {
			if (sscanf(buf, "%hx", &p_pcidev->vendor) != 1)
				p_pcidev->vendor = 0;
			PRINT_DBG("vendor: 0x%04x\n", p_pcidev->vendor);
		}
		ret = topo_path_read_buffer(dir, "device", buf,
					    WAYCA_SC_ATTR_STRING_LEN);
		if (ret <= 0)
			p_pcidev->device = 0;
		else {
			if (sscanf(buf, "%hx", &p_pcidev->device) != 1)
				p_pcidev->device = 0;
			PRINT_DBG("device: 0x%04x\n", p_pcidev->device);
		}
		/* read local_cpulist */
		p_pcidev->local_cpu_map = CPU_ALLOC(p_topo->kernel_max_cpus);
		if (!p_pcidev->local_cpu_map)
			return -ENOMEM;
		ret = topo_path_read_cpulist(dir, "local_cpulist",
					     p_pcidev->local_cpu_map,
					     p_topo->kernel_max_cpus);
		if (ret != 0) {
			PRINT_ERROR("Failed to get local_cpulist, ret = %d\n",
				    ret);
			return ret;
		}

		/* read irqs */
		ret = topo_parse_device_irqs(dir, &p_pcidev->irqs);
		if (ret < 0) {
			PRINT_ERROR("Failed in topo_parse_device_irqs: %s\n",
				    dir);
			PRINT_ERROR("\t Error code: %d\n", ret);
			return ret;
		}

		/* read enable */
		ret = topo_path_read_s32(dir, "enable", &p_pcidev->enable);
		if (ret < 0) {
			PRINT_ERROR("Failed to read %s/enable\n", dir);
			PRINT_ERROR("\t Error code: %d\n", ret);
			return ret;
		}
		/* read smmu link */
		char buf_link[WAYCA_SC_PATH_LEN_MAX];
		char path_buffer[WAYCA_SC_PATH_LEN_MAX];

		p_pcidev->smmu_idx = -1; /* initialize */
		sprintf(path_buffer, "%s/iommu", dir);
		if (readlink(path_buffer, buf_link, WAYCA_SC_PATH_LEN_MAX) ==
		    -1) {
			if (errno == ENOENT)
				PRINT_DBG(" No IOMMU\n");
			else {
				PRINT_ERROR(
					"Failed to read iommu. Error code: %d\n",
					-errno);
				return -errno;
			}
		} else {
			/* extract smmu index from buf_link */
			PRINT_DBG("iommu link: %s\n", buf_link);
			/* example:
			 * ../../platform/arm-smmu-v3.3.auto/iommu/smmu3.0x0000000140000000 */
			/* TODO: here is hard-coded name */
			p_index = strstr(buf_link, "arm-smmu-v3");

			/* what to do in other versions of smmu? */
			if (p_index == NULL)
				PRINT_ERROR("Failed to parse iommu link: %s\n",
					    buf_link);
			else {
				p_pcidev->smmu_idx = strtol(
					p_index + strlen("arm-smmu-v3") + 1,
					NULL, 0);
				PRINT_DBG("smmu index: %d\n",
					  p_pcidev->smmu_idx);
			}
		}
		return 0;
	} else if (strstr(dir, "smmu")) {
		PRINT_DBG("SMMU full path: %s\n", dir);
		p_smmu = (struct wayca_smmu *)calloc(1,
						     sizeof(struct wayca_smmu));
		if (!p_smmu)
			return -ENOMEM;

		/* read numa_node */
		topo_path_read_s32(dir, "numa_node", &node_nb);
		PRINT_DBG("numa_node: %d\n", node_nb);
		if (node_nb < 0)
			node_nb = 0; /* on failure, default to node #0 */
		p_smmu->numa_node = node_nb;

		/* get the 'wayca_node *' whoes node_idx == this 'node_nb' */
		for (i = 0; i < p_topo->n_nodes; i++) {
			if (p_topo->nodes[i]->node_idx == node_nb)
				break;
		}
		if (i == p_topo->n_nodes) {
			PRINT_ERROR(
				"Failed to match this PCI device to any numa node: %s",
				dir);
			free(p_smmu);
			return -1;
		}
		/* append p_smmu to 'wayca node *'->smmus */
		struct wayca_smmu **p_temp;

		p_temp = (struct wayca_smmu **)realloc(
			p_topo->nodes[i]->smmus,
			(p_topo->nodes[i]->n_smmus + 1) *
				sizeof(struct wayca_smmu *));
		if (!p_temp) {
			free(p_smmu);
			return -ENOMEM;
		}
		p_topo->nodes[i]->smmus = p_temp;
		p_topo->nodes[i]->smmus[p_topo->nodes[i]->n_smmus] = p_smmu;
		p_topo->nodes[i]
			->n_smmus++; /* incement number of SMMU devices */
		PRINT_DBG("n_smmus = %zu\n", p_topo->nodes[i]->n_smmus);

		/* read SMMU information into: p_smmu */
		/* read type (modalias) */
		ret = topo_path_read_buffer(dir, "modalias", p_smmu->modalias,
					    WAYCA_SC_ATTR_STRING_LEN);
		if (ret <= 0)
			p_smmu->modalias[0] = '\0';
		else if (p_smmu->modalias[strlen(p_smmu->modalias) - 1] == '\n')
			p_smmu->modalias[strlen(p_smmu->modalias) - 1] = '\0';
		PRINT_DBG("modalias = %s\n", p_smmu->modalias);

		/* identify smmu_idx from the 'dir' string */
		/*  Eg. SMMU full path: /sys/devices/platform/arm-smmu-v3.4.auto */
		/* TODO: here is hard-coded name */
		p_index = strstr(dir, "arm-smmu-v3");
		/* what to do in other versions of smmu? */
		if (p_index == NULL)
			PRINT_ERROR("Failed to parse smmu_idx : %s\n", dir);
		else {
			p_smmu->smmu_idx = strtol(
				p_index + strlen("arm-smmu-v3") + 1, NULL, 0);
			PRINT_DBG("smmu index: %d\n", p_smmu->smmu_idx);
		}
		/* read base address */
		sprintf(path_buffer, "%s/iommu", dir);
		dp = opendir(path_buffer);
		if (!dp)
			return -errno;

		while ((entry = readdir(dp)) != NULL) {
			/*
			 * Found a directory, eg. iommu/smmu3.0x0000000140000000
			 */
			p_index = strstr(entry->d_name, "smmu3");
			if (p_index) {
				p_smmu->base_addr = strtoull(
					p_index + strlen("smmu3") + 1, NULL, 0);
				PRINT_DBG("base address : 0x%016llx\n",
					  p_smmu->base_addr);
				break;
			}
			continue;
		}
		return 0;
	} else {
		/* TODO: support for other device */
		PRINT_DBG("other IO device at full path: %s\n", dir);
	}

	return 0;
}

/* Return negative on error, 0 on success
 */
static int topo_recursively_read_io_devices(const char *rootdir,
					    struct wayca_topo *p_topo)
{
	DIR *dp;
	struct dirent *entry;
	struct stat statbuf;
	char cwd[WAYCA_SC_PATH_LEN_MAX];

	dp = opendir(rootdir);
	if (!dp)
		return -errno;

	chdir(rootdir);
	while ((entry = readdir(dp)) != NULL) {
		lstat(entry->d_name, &statbuf);
		if (S_ISDIR(statbuf.st_mode)) {
			/* Found a directory, but ignore . and .. */
			if (strcmp(".", entry->d_name) == 0 ||
			    strcmp("..", entry->d_name) == 0)
				continue;
			topo_recursively_read_io_devices(entry->d_name, p_topo);
		} else {
			/*
			 * TODO: We rely on 'numa_node' to represent a
			 * legitimate i/o device. However 'numa_node' exists
			 * only when NUMA is enabled in kernel. So, we Need to
			 * consider a better idea of identifying i/o device.
			 */
			if (strcmp("numa_node", entry->d_name) == 0) {
				topo_parse_io_device(
					getcwd(cwd, WAYCA_SC_PATH_LEN_MAX),
					p_topo);
			}
		}
	}

	chdir("..");
	closedir(dp);
	return 0;
}

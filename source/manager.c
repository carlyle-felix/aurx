#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include "../include/manager.h"
#include "../include/list.h"
#include "../include/util.h"
#include "../include/memory.h"

// prototypes (NOTE: A lot of these must be moved. kept here during testing)
void rm_depends(alpm_handle_t *local, alpm_pkg_t *pkg);
void clean_up(Depends *rm_list);
void list_free(char *data);		// for alpm_list_fn_free
alpm_list_t *alpm_local(alpm_handle_t **local, alpm_errno_t *err);
alpm_list_t *alpm_repos(alpm_handle_t **repo);
bool is_foreign(char *pkgname);
bool is_installed(char *pkgname);
int install_depends(Depends *deps);
void resolve_deps(alpm_handle_t *local, alpm_list_t *repo_db_list, alpm_pkg_t *pkg);
char *read_srcinfo(char *pkgname);
Srcinfo *pkg_srcinfo_malloc(void);
char *zst_path(Srcinfo *pkg);
Depends *add_data(Depends *list, const char *data);
Depends *depends_malloc(void);
void clear_depends(Depends *list);
int rm_makedepends(Depends *deps);

// return list of packages in localdb - probably don't need this.
alpm_list_t *alpm_local(alpm_handle_t **local, alpm_errno_t *err) {

    alpm_db_t *local_db;
    alpm_list_t *local_list;

    *local = alpm_initialize("/", "/var/lib/pacman", err);
    local_db = alpm_get_localdb(*local);
    local_list = alpm_db_get_pkgcache(local_db);

    return local_list; 
}

// list of repos in pacman.conf
alpm_list_t *alpm_repos(alpm_handle_t **repo) {

	pu_config_t *conf;
	alpm_db_t *repos_db;
	alpm_list_t *repo_list;

	conf = pu_config_new();
    pu_ui_config_load(conf, "/etc/pacman.conf");
    *repo = pu_initialize_handle_from_config(conf);
	if (*repo == NULL) {
		printf("pu_initialize_buffer_from_config failed.\n");
	}
    repo_list = pu_register_syncdbs(*repo, conf->repos);

    pu_config_free(conf);

	return repo_list;
}

List *foreign_list(void) {
    
    alpm_handle_t *local = NULL, *repo = NULL;
    alpm_errno_t err;
    alpm_list_t *local_list, *repo_list, *reset;
    alpm_pkg_t *pkg;
    List *aur;
   
	repo_list = alpm_repos(&repo);	
    local_list = alpm_local(&local, &err);
    
    aur = list_malloc();
    for (reset = repo_list; local_list != NULL; local_list = alpm_list_next(local_list)) {
        for (repo_list = reset; repo_list != NULL; repo_list = alpm_list_next(repo_list)) {
            pkg = alpm_db_get_pkg(repo_list->data, alpm_pkg_get_name(local_list->data));
            if (pkg != NULL) {
                break;
            }
        }    

        if (pkg == NULL) {
            add_pkgname(aur, alpm_pkg_get_name(local_list->data));
            add_pkgver(aur, alpm_pkg_get_name(local_list->data), alpm_pkg_get_version(local_list->data));
        }
    }

    alpm_release(local);
    alpm_release(repo);

    return aur;
}

bool is_foreign(char *pkgname) {

	List *list, *temp;
	
	list = foreign_list();
	for (temp = list; temp != NULL; temp = temp->next) {
		if (strcmp(temp->pkgname, pkgname) == 0) {
			break;
		}
	}
	clear_list(list);
	return (temp != NULL);
}

bool is_installed(char *pkgname) {

	alpm_handle_t *local;
	alpm_db_t *local_db;
	alpm_errno_t err;
	alpm_pkg_t *pkg;
	bool installed = true;

	alpm_local(&local, &err);
	local_db = alpm_get_localdb(local);
	pkg = alpm_db_get_pkg(local_db, pkgname);
	if (pkg == NULL) {
		installed = false;
	}
	alpm_release(local);

	return installed;
}

void list_free(char *data) {
	
	if (data == NULL){
		return;
	}
	free(data);
}

/*	
 * 	check if any packages besides the one being removed requires one of its deps
 * 	before removing the dep, if required the dep is still needed return true.
 * 
 * 	NOTE: This function and alpm_uninstall() should be rewritten to have separated
 * 	transactions in order for this funtion to be reused by alpm_install to remove 
 * 	makedepends.
 */
void rm_depends(alpm_handle_t *local, alpm_pkg_t *pkg) {
	
	alpm_db_t *local_db;
	alpm_pkg_t *dep_pkg;
	alpm_depend_t *dep;
	alpm_list_t *dep_list, *req_list, *temp;

	local_db = alpm_get_localdb(local);
	
	// search localdb for pkgname and use it to get a list of
	// depenencies for package and traverse the list.
	dep_list = alpm_pkg_get_depends(pkg);
	for (dep_list; dep_list != NULL; dep_list = alpm_list_next(dep_list)) {
		dep = dep_list->data;

		// find the dependency in the localdb
		dep_pkg = alpm_db_get_pkg(local_db, dep->name);

		// get a list of packages that requires the dependency
		req_list = alpm_pkg_compute_requiredby(dep_pkg);
		
		// check if theres only one package in the required list
		// if this is true, there are no other packages that requires it.
		if (alpm_list_count(req_list) == 1 && strcmp(req_list->data, alpm_pkg_get_name(pkg)) == 0) {
			if (alpm_pkg_get_reason(dep_pkg) == ALPM_PKG_REASON_DEPEND) {
				alpm_remove_pkg(local, dep_pkg);
			}
			rm_depends(local, dep_pkg);
		}

		alpm_list_free_inner(req_list, (alpm_list_fn_free) list_free);
		alpm_list_free(req_list);
	}
}


int alpm_uninstall(List *pkglist) {

	alpm_handle_t *local;
	alpm_db_t *local_db;
	alpm_pkg_t *pkg;
	alpm_list_t *list, *temp_list, *opt, *error_list;
	alpm_errno_t err;
	List *temp;
	Depends *post_rm = NULL;
	const char *pkgname;
	bool proceed = true, success = true;
	int res;

	list = alpm_local(&local, &err);
	local_db = alpm_get_localdb(local);
	
	gain_root();
	res = alpm_trans_init(local, ALPM_TRANS_FLAG_CASCADE | ALPM_TRANS_FLAG_NODEPVERSION);
	if (res != 0) {
		printf(BOLD"Elevated privilage required to perform this operation ("BRED"root"BOLD").\n"RESET);
		printf(BRED"error:"RESET" alpm_trans_init: %s\n", alpm_strerror(alpm_errno(local)));
	}
	drop_root();

	printf("Checking dependencies...\n\n");
	for (temp = pkglist; temp != NULL; temp = temp->next) {
		if (is_installed(temp->pkgname) == false) {
			printf(BRED"error:"RESET" target not found: %s.\n", temp->pkgname);
			proceed = false;
			break;
		} else if (is_foreign(temp->pkgname) == false) {
			printf(BRED"error:"RESET" %s is not an AUR package.\n", temp->pkgname);
			proceed = false;
			break;
		}
		
		pkg = alpm_db_get_pkg(local_db, temp->pkgname);
		if (pkg == NULL) {
			printf(BRED"error:"RESET" target not found in local_db: %s.\n", temp->pkgname);
			proceed = false;
			continue;
		}
		rm_depends(local, pkg);
		res = alpm_remove_pkg(local, pkg);
		if (res != 0) {
			printf(BRED"error:"RESET" alpm_remove_pkg: %s\n", alpm_strerror(alpm_errno(local)));
		}
	}

	if (proceed == false) {
		gain_root();
		alpm_trans_release(local);
		drop_root();
		alpm_release(local);
		return -1;
	}
	
	list = alpm_trans_get_remove(local);
	printf(BOLD"Packages (%d): "RESET, alpm_list_count(list));
	for (; list != NULL; list = alpm_list_next(list)) {
		pkgname = alpm_pkg_get_name(list->data);
		printf("%s"GREY"-%s  "RESET, pkgname, alpm_pkg_get_version(list->data));
		post_rm = add_data(post_rm, pkgname);
	}

	printf("\n\n"BBLUE"::"BOLD" Do you want to remove these packages? [Y/n] "RESET);
	if (prompt() == false) {
		alpm_trans_release(local);
		alpm_release(local);
		return -1;
	}

	gain_root();
	res = alpm_trans_prepare(local, &error_list);
	if (res != 0) {
		printf(BRED"error:"RESET" alpm_trans_prepare: %s\n", alpm_strerror(alpm_errno(local)));
	}
	res = alpm_trans_commit(local, &error_list);
	if (res != 0) {
		printf(BRED"error:"RESET" alpm_trans_commit: %s\n", alpm_strerror(alpm_errno(local)));
	}
	if (res == 0) {
		clean_up(post_rm);
		printf(BGREEN"=>"BOLD" Success\n"RESET);
	}
	clear_depends(post_rm);
	alpm_trans_release(local);	
	drop_root();
	alpm_release(local);
}

// cleaning up system files not needed, pivot to cleaning config and cache dirs. 
void clean_up(Depends *post_rm) {

	const char *pkgname;
	char config[MAX_BUFFER], cache[MAX_BUFFER];

	change_dir("HOME");
	
	for (; post_rm != NULL; post_rm = post_rm->next) {
		strcpy(config, ".config/");
		strcpy(cache, ".cache/");

		pkgname = post_rm->data;
		strcat(config, pkgname);
		if (is_dir(config) == true) {
			remove_dir(config);
		}
		strcat(cache, pkgname);
		if (is_dir(cache) == true) {
			remove_dir(cache);
		}
	}
	
	change_dir("WD");
}

int alpm_install(List *list) {

	alpm_handle_t *local;
	alpm_list_t *add_list, *error_list, *repo_db_list;
	alpm_pkg_t *pkg;
	Srcinfo *pkg_info;
	int res;
	bool ans = true;

	repo_db_list = alpm_repos(&local);

	// depends and makedepends must be installed before package.
	for (;list != NULL; list = list->next) {
		// ignore packages user rejected
		if (list->install == false) {
			continue;
		}
		// ignore packages that are already installed
		if (is_installed(list->pkgname) == true) {
			printf(BOLD"%s already installed.\n"RESET, list->pkgname);
			continue;
		}

		pkg_info = populate_pkg(list->pkgname);

		printf(BGREEN"==>"BOLD" Checking dependencies...\n\n"RESET);
		res = install_depends(pkg_info->depends);
		if (res != 0) {
			printf(BRED"error:"RESET" failed to install dependencies\n");
			clear_pkg_srcinfo(pkg_info);
			continue;
		}
		res = install_depends(pkg_info->makedepends);
		if (res != 0) {
			printf(BRED"error:"RESET" failed to install build dependencies\n");
			clear_pkg_srcinfo(pkg_info);
			continue;
		}

		// build and add the package zst
		gain_root();
		res = build(list->pkgname);
		if (res != 0) {
			printf(BRED"error:"RESET" failed to build package: %s", list->pkgname);
		}
		drop_root();

		// remove makedepends
		res = rm_makedepends(pkg_info->makedepends);
		if (res != 0) {
			printf("Keeping makedepends.");
		}

		// this should be done after deps are resolved.
		gain_root();
		res = alpm_trans_init(local, ALPM_TRANS_FLAG_NODEPVERSION);
		if (res != 0) {
			printf("error: trans_init: %s\n", alpm_strerror(alpm_errno(local)));
		}
		drop_root();
		
		res = alpm_pkg_load(local, pkg_info->zst_path, 1, 0, &pkg);
		if (res != 0) {
			printf(BRED"error:"RESET" failed to add local package: %s.\n", alpm_strerror(alpm_errno(local)));
		}
		res = alpm_add_pkg(local, pkg);
		if (res != 0) {
			printf(BRED"error:"RESET" failed to add package.\n");
		}
		clear_pkg_srcinfo(pkg_info);

		// print package list
		add_list = alpm_trans_get_add(local);
		printf(BOLD"\nPackages (%d): "RESET, alpm_list_count(add_list));
		while (add_list != NULL) {
			printf("%s"GREY"-%s  "RESET, alpm_pkg_get_name(add_list->data), alpm_pkg_get_version(add_list->data));
			add_list = alpm_list_next(add_list);
		}

		printf(BBLUE"\n\n::"BOLD"Proceed with installation? [Y/n] "RESET);
		ans = prompt();	
		gain_root();

		if (ans == false) {
			alpm_trans_release(local);
			drop_root();
			alpm_release(local);
			return -1;
		}
		res = alpm_trans_prepare(local, &error_list);
		if (res != 0) {
			printf(BRED"error:"RESET" trans_prep: %s\n", alpm_strerror(alpm_errno(local)));
		}
		res = alpm_trans_commit(local, &error_list);
		if (res != 0) {
			printf(BRED"error:"RESET" trans_commit: %s\n", alpm_strerror(alpm_errno(local)));
		}
		if (res == 0) {
			printf(BGREEN"==>"BOLD" Success\n"RESET);
		}
		alpm_trans_release(local);
		drop_root();
		alpm_release(local);
	}	
	return 0;
}

int install_depends(Depends *deps) {

	alpm_handle_t *local;
	alpm_list_t *repo_db_list, *db_temp, *add_list, *error_list;
	alpm_pkg_t *pkg;
	Depends *temp_deps;
	bool ans = true, missing_dep = false;
	int res;

	if (deps == NULL) {
		return 0;
	}

	repo_db_list = alpm_repos(&local);

	gain_root();
	res = alpm_trans_init(local, ALPM_TRANS_FLAG_ALLDEPS | ALPM_TRANS_FLAG_NODEPVERSION);
	if (res != 0) {
		printf(BRED"error:"RESET" trans_init: %s\n", alpm_strerror(alpm_errno(local)));
	}
	drop_root();

	// pass deps one by one to resolve_deps
	for (temp_deps = deps; temp_deps != NULL; temp_deps = temp_deps->next) {
		for (db_temp = repo_db_list; db_temp != NULL; db_temp = alpm_list_next(db_temp)) {
			pkg = alpm_db_get_pkg(db_temp->data, temp_deps->data);
			if (pkg != NULL) {
				if (is_installed(temp_deps->data) == false) {
					missing_dep = true;
					res = alpm_add_pkg(local, pkg);
					if (res != 0) {
						printf(BRED"error:"RESET" alpm_add_pkg (install): %s\n", alpm_strerror(alpm_errno(local)));
					}
				} 
				resolve_deps(local, repo_db_list, pkg);
			}
		}
	}

	if (missing_dep == false) {
		gain_root();
		alpm_trans_release(local);
		drop_root();
		alpm_release(local);
		return 0;
	}

	// print package list
	add_list = alpm_trans_get_add(local);
	printf(BOLD"\nPackages (%d): "RESET, alpm_list_count(add_list));
	while (add_list != NULL) {
		printf("%s"GREY"-%s  "RESET, alpm_pkg_get_name(add_list->data), alpm_pkg_get_version(add_list->data));
		add_list = alpm_list_next(add_list);
	}

	// dont gain root before prompt.
	printf(BBLUE"\n\n::"BOLD"Proceed with installation? [Y/n] "RESET);
	ans = prompt();
	gain_root();
	if (ans == false) {
		alpm_trans_release(local);
		drop_root();
		alpm_release(local);
		return -1;
	}
	
	res = alpm_trans_prepare(local, &error_list);
	printf("prep res: %d\n", res);
	if (res != 0) {
		printf(BRED"error:"RESET" trans_prep: %s\n", alpm_strerror(alpm_errno(local)));
	}
	res = alpm_trans_commit(local, &error_list);
	if (res != 0) {
		printf(BRED"error:"RESET" trans_commit: %s\n", alpm_strerror(alpm_errno(local)));
	}
	if (res == 0) {
		printf(BGREEN"==>"BOLD" Success\n"RESET);
	}
	alpm_trans_release(local);
	drop_root();
	alpm_release(local);	
	
	return 0;
}

int rm_makedepends(Depends *deps) {
	
	alpm_handle_t *local;
	alpm_db_t *local_db;
	alpm_list_t *repo_db_list, *list, *error_list;
	alpm_pkg_t *pkg;
	int res;

	if (deps == NULL) {
		return 0;
	}
	
	repo_db_list = alpm_repos(&local);
	local_db = alpm_get_localdb(local);

	gain_root();
	res = alpm_trans_init(local, ALPM_TRANS_FLAG_CASCADE | ALPM_TRANS_FLAG_NODEPVERSION);
	if (res != 0) {
		printf(BRED"error:"RESET" trans_init: %s\n", alpm_strerror(alpm_errno(local)));
	}
	drop_root();

	for (; deps != NULL; deps = deps->next) {
		pkg = alpm_db_get_pkg(local_db, deps->data);
		rm_depends(local, pkg);
	}

	list = alpm_trans_get_remove(local);
	printf(BOLD"Packages (%d): "RESET, alpm_list_count(list));
	for (; list != NULL; list = alpm_list_next(list)) {
		printf("%s"GREY"-%s  "RESET, alpm_pkg_get_name(list->data), alpm_pkg_get_version(list->data));
	}

	printf("\n\n"BBLUE"::"BOLD" Do you want to remove these packages? [Y/n] "RESET);
	if (prompt() == false) {
		gain_root();
		alpm_trans_release(local);
		drop_root();
		alpm_release(local);
		return -1;
	}

	gain_root();
	res = alpm_trans_prepare(local, &error_list);
	if (res != 0) {
		printf(BRED"error:"RESET" alpm_trans_prepare: %s\n", alpm_strerror(alpm_errno(local)));
	}
	res = alpm_trans_commit(local, &error_list);
	if (res != 0) {
		printf(BRED"error:"RESET" alpm_trans_commit: %s\n", alpm_strerror(alpm_errno(local)));
	}
	if (res == 0) {
		printf(BGREEN"=>"BOLD" Success\n"RESET);
	}
	alpm_trans_release(local);	
	drop_root();

	alpm_release(local);
	return 0;
}

/*
*	add dependencies to transaction
*/
void resolve_deps(alpm_handle_t *local, alpm_list_t *repo_db_list, alpm_pkg_t *pkg) {

	alpm_list_t *db_temp, *req_deps;
	alpm_pkg_t *dep_pkg;
	alpm_depend_t *dep;
	int res;

	req_deps = alpm_pkg_get_depends(pkg);

	for (; req_deps != NULL; req_deps = alpm_list_next(req_deps)) {
		dep = req_deps->data;
		for (db_temp = repo_db_list; db_temp != NULL; db_temp = alpm_list_next(db_temp)) {
			dep_pkg = alpm_db_get_pkg(db_temp->data, dep->name);
			if (dep_pkg != NULL && is_installed(dep->name) == false) {
				res = alpm_add_pkg(local, dep_pkg);
				if (res != 0) {
					printf("error: alpm_add_pkg: %s\n", alpm_strerror(alpm_errno(local)));
				}

				alpm_pkg_set_reason(dep_pkg, ALPM_PKG_REASON_DEPEND);
				resolve_deps(local, repo_db_list, dep_pkg);
			}
		}
	}
}

/*
*	populate package struct.
*/
Srcinfo *populate_pkg(char *pkgname) {

	Srcinfo *pkg = pkg_srcinfo_malloc();
	int key_len;
	register int i, key;
	char *buffer, *temp_buffer, *str, key_item[MAX_BUFFER];
	char *keys[] = {"pkgname", "epoch", "pkgver", "pkgrel", 
					"makedepends", "depends", "optdepends"};

	buffer = read_srcinfo(pkgname);

	// traverse list of keys
	for (key = 0; key < 7; key++) {
		key_len = strlen(keys[key]);
		for (temp_buffer = buffer; *temp_buffer != '\0'; temp_buffer++) {
			// advance past the tab.
			if (*temp_buffer == '\t') {
				temp_buffer++;
			}

			// check the first letter, if no match, advance to newline char.
			if (*temp_buffer != keys[key][0]) {
				while (*temp_buffer != '\n') {
					temp_buffer++;
				}
				continue;
			}

			for (i = 0; i < key_len; i++) {
				if (*temp_buffer++ != keys[key][i]) {
					// if no match, skip line, advance to newline char.
					while (*temp_buffer != '\n') {
						temp_buffer++;
					}
					i = 0;
					break;
				}
			}
			
			if (i == (key_len)) {
				key_item[0] = '\0';
				while (*temp_buffer == ' ' || *temp_buffer == '=') {
					temp_buffer++;
				}
				for (i = 0; *temp_buffer != '\n' && *temp_buffer != '>'; i++) {
					key_item[i] = *temp_buffer++;
				}
				key_item[i] = '\0';
				if (*temp_buffer == '>') {
					while (*temp_buffer != '\n') {
						temp_buffer++;
					}
				}
				if (key_item[0] == '\0') {
					continue;
				}
				// put data in correct fields according to key.
				switch (key) {
					case 0:		
						str_alloc(&pkg->pkgname, strlen(key_item) + 1);
						strcpy(pkg->pkgname, key_item);
						break;
					case 1:
						str_alloc(&pkg->epoch, strlen(key_item) + 1);
						strcpy(pkg->epoch, key_item);
						break;
					case 2:
						str_alloc(&pkg->pkgver, strlen(key_item) + 1);
						strcpy(pkg->pkgver, key_item);
						break;
					case 3: 
						str_alloc(&pkg->pkgrel, strlen(key_item) + 1);
						strcpy(pkg->pkgrel, key_item);
						break;
					case 4:
						pkg->makedepends = add_data(pkg->makedepends, key_item);
						break;
					case 5: 
						pkg->depends = add_data(pkg->depends, key_item);
						break;
					case 6:
						pkg->optdepends = add_data(pkg->optdepends, key_item);
						break;
					default:
						break;
				}
			}
		}
	}
	pkg->zst_path = zst_path(pkg);

	change_dir("WD");
	free(buffer);

	return pkg;
}

/*
 * returns a list of keys found.
 * pkgname: as found in .SRCINFO.
 * key: .SRCINFO field to search for.
 * TODO: deal with version requirements.
 */
char *read_srcinfo(char *pkgname) {

	FILE *srcinfo; 
	char *buffer = NULL;
	int read = 0, max = MAX_BUFFER;
	
	change_dir(pkgname);

	str_alloc(&buffer, max);
	for (;;) {
		srcinfo = fopen(".SRCINFO", "r");
		if (srcinfo == NULL) {
			printf(BRED"error:"RESET" failed to open %s/.SRCINFO", pkgname);
		}
		read = fread(buffer, sizeof(char), max, srcinfo);
		if (read == max) {
			max = read * 2;
			str_alloc(&buffer, max);
		} else {
			fclose(srcinfo);
			break;
		}
		fclose(srcinfo);
	}
	buffer[read] = '\0';

	return buffer;
}

// Move to memory

/* 	char *pkgname;
 *  char *epoch;
 *  char *pkgver;
 *  char *pkgrel;
 *  char *zst_path;
 *  Depends *makedepends;
 *  Depends *depends;
 *  Depends *optdepends;
*/

/*
*	return the absolute path of package zst 
*/
char *zst_path(Srcinfo *pkg) {
	
	char *cwd, *path = NULL;

	cwd = change_dir(pkg->pkgname);

	if (pkg->epoch == NULL) {
		str_alloc(&path, strlen(cwd) + strlen(pkg->pkgname) + strlen(pkg->pkgver) + strlen(pkg->pkgrel) + 24);
		sprintf(path, "%s/%s-%s-%s-x86_64.pkg.tar.zst", cwd, pkg->pkgname, pkg->pkgver, pkg->pkgrel);
	} else if (pkg->pkgrel == NULL) {
		str_alloc(&path, strlen(cwd) + strlen(pkg->pkgname) + strlen(pkg->epoch) + strlen(pkg->pkgver) + 24);
		sprintf(path, "%s/%s-%s:%s-x86_64.pkg.tar.zst", cwd, pkg->pkgname, pkg->epoch, pkg->pkgver);
	} else if (pkg->epoch == NULL && pkg->pkgrel == NULL) {
		str_alloc(&path, strlen(cwd) + strlen(pkg->pkgname) + strlen(pkg->pkgver) + 23);
		sprintf(path, "%s/%s-%s-x86_64.pkg.tar.zst", cwd, pkg->pkgname, pkg->pkgver);
	} else {
		str_alloc(&path, strlen(cwd) + strlen(pkg->pkgname) + strlen(pkg->epoch) + strlen(pkg->pkgver) + strlen(pkg->pkgrel) + 25);
		sprintf(path, "%s/%s-%s:%s-%s-x86_64.pkg.tar.zst", cwd, pkg->pkgname, pkg->epoch, pkg->pkgver, pkg->pkgrel);
	}

	return path;
}

Srcinfo *pkg_srcinfo_malloc(void) {

	Srcinfo *pkg = malloc(sizeof(Srcinfo));
	if (pkg == NULL) {
		printf(BRED"error:"RESET" failed to allocate memory for srcinfo list.\n");
		exit(EXIT_FAILURE);
	}
	pkg->pkgname = NULL;
	pkg->epoch = NULL;
	pkg->pkgver = NULL;
	pkg->pkgrel = NULL;
	pkg->zst_path = NULL;
	pkg->makedepends = NULL;
	pkg->depends = NULL;
	pkg->optdepends = NULL;

	return pkg;
}

void clear_pkg_srcinfo(Srcinfo *pkg) {

	free(pkg->pkgname);
	free(pkg->epoch);
	free(pkg->pkgver);
	free(pkg->pkgrel);
	free(pkg->zst_path);
	clear_depends(pkg->makedepends);
	clear_depends(pkg->depends);
	clear_depends(pkg->optdepends);
	free(pkg);
	
}

Depends *depends_malloc(void) {

	Depends *temp;

	temp = malloc(sizeof(Depends));
	if (temp == NULL) {
		printf(BRED"error:"RESET" failed to allocate storage for data_list\n");
		exit(EXIT_FAILURE);
	}
	temp->data = NULL;
	temp->next = NULL;

	return temp;
}

Depends *add_data(Depends *list, const char *data) {
	
	Depends *temp;

	temp = depends_malloc();
	str_alloc(&temp->data, strlen(data) + 1);
	strcpy(temp->data, data);
	temp->next = list;
	list = temp;

	return list;
}

void clear_depends(Depends *list) {

	Depends *temp_list;

	while (list != NULL) {
		temp_list = list;
		list = list->next;
		free(temp_list->data);
		free(temp_list);
	}
}
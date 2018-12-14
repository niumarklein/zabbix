/*
** Zabbix
** Copyright (C) 2001-2018 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "lld.h"
#include "db.h"
#include "log.h"
#include "events.h"
#include "zbxalgo.h"
#include "zbxserver.h"
#include "zbxregexp.h"

/* lld rule filter condition (item_condition table record) */
typedef struct
{
	zbx_uint64_t		id;
	char			*macro;
	char			*regexp;
	zbx_vector_ptr_t	regexps;
	unsigned char		op;
}
lld_condition_t;

/* lld rule filter */
typedef struct
{
	zbx_vector_ptr_t	conditions;
	char			*expression;
	int			evaltype;
}
lld_filter_t;

typedef struct
{
	char	*lld_macro;
	char	*path;
}
zbx_lld_macro_path_t;

/******************************************************************************
 *                                                                            *
 * Function: lld_macro_paths_compare                                          *
 *                                                                            *
 * Purpose: sorting function to sort LLD macros by unique name                *
 *                                                                            *
 ******************************************************************************/
static int	lld_macro_paths_compare(const void *d1, const void *d2)
{
	const zbx_lld_macro_path_t	*r1 = *(const zbx_lld_macro_path_t **)d1;
	const zbx_lld_macro_path_t	*r2 = *(const zbx_lld_macro_path_t **)d2;

	return strcmp(r1->lld_macro, r2->lld_macro);
}

/******************************************************************************
 *                                                                            *
 * Function: lld_macro_paths_get                                              *
 *                                                                            *
 * Purpose: retrieve list of LLD macros                                       *
 *                                                                            *
 * Parameters: lld_ruleid      - [IN] LLD id                                  *
 *             lld_macro_paths - [OUT] use json path to extract from jp_row   *
 *             error           - [OUT] in case json path is invalid           *
 *                                                                            *
 ******************************************************************************/
static int	lld_macro_paths_get(zbx_uint64_t lld_ruleid, zbx_vector_ptr_t *lld_macro_paths, char **error)
{
	const char		*__function_name = "lld_macro_paths_get";

	DB_RESULT		result;
	DB_ROW			row;
	zbx_lld_macro_path_t	*lld_macro_path;
	int			ret = SUCCEED;
	char			err[MAX_STRING_LEN];

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	result = DBselect(
			"select lld_macro,path"
			" from lld_macro_path"
			" where itemid=" ZBX_FS_UI64
			" order by lld_macro",
			lld_ruleid);

	while (NULL != (row = DBfetch(result)))
	{
		if (SUCCEED != (ret = zbx_json_path_check(row[1], err, sizeof(err))))
		{
			*error = zbx_dsprintf(*error, "Cannot process LLD macro \"%s\": %s.\n", row[0], err);
			break;
		}

		lld_macro_path = (zbx_lld_macro_path_t *)zbx_malloc(NULL, sizeof(zbx_lld_macro_path_t));

		lld_macro_path->lld_macro = zbx_strdup(NULL, row[0]);
		lld_macro_path->path = zbx_strdup(NULL, row[1]);

		zbx_vector_ptr_append(lld_macro_paths, lld_macro_path);
	}
	DBfree_result(result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: lld_macro_path_free                                              *
 *                                                                            *
 * Purpose: release resources allocated by lld macro path                     *
 *                                                                            *
 * Parameters: lld_macro_path - [IN] json path to extract from lld_row        *
 *                                                                            *
 ******************************************************************************/
static void	lld_macro_path_free(zbx_lld_macro_path_t *lld_macro_path)
{
	zbx_free(lld_macro_path->path);
	zbx_free(lld_macro_path->lld_macro);
	zbx_free(lld_macro_path);
}

/******************************************************************************
 *                                                                            *
 * Function: lld_condition_free                                               *
 *                                                                            *
 * Purpose: release resources allocated by filter condition                   *
 *                                                                            *
 * Parameters: condition  - [IN] the filter condition                         *
 *                                                                            *
 ******************************************************************************/
static void	lld_condition_free(lld_condition_t *condition)
{
	zbx_regexp_clean_expressions(&condition->regexps);
	zbx_vector_ptr_destroy(&condition->regexps);

	zbx_free(condition->macro);
	zbx_free(condition->regexp);
	zbx_free(condition);
}

/******************************************************************************
 *                                                                            *
 * Function: lld_conditions_free                                              *
 *                                                                            *
 * Purpose: release resources allocated by filter conditions                  *
 *                                                                            *
 * Parameters: conditions - [IN] the filter conditions                        *
 *                                                                            *
 ******************************************************************************/
static void	lld_conditions_free(zbx_vector_ptr_t *conditions)
{
	zbx_vector_ptr_clear_ext(conditions, (zbx_clean_func_t)lld_condition_free);
	zbx_vector_ptr_destroy(conditions);
}

/******************************************************************************
 *                                                                            *
 * Function: lld_condition_compare_by_macro                                   *
 *                                                                            *
 * Purpose: compare two filter conditions by their macros                     *
 *                                                                            *
 * Parameters: item1  - [IN] the first filter condition                       *
 *             item2  - [IN] the second filter condition                      *
 *                                                                            *
 ******************************************************************************/
static int	lld_condition_compare_by_macro(const void *item1, const void *item2)
{
	lld_condition_t	*condition1 = *(lld_condition_t **)item1;
	lld_condition_t	*condition2 = *(lld_condition_t **)item2;

	return strcmp(condition1->macro, condition2->macro);
}

/******************************************************************************
 *                                                                            *
 * Function: lld_filter_init                                                  *
 *                                                                            *
 * Purpose: initializes lld filter                                            *
 *                                                                            *
 * Parameters: filter  - [IN] the lld filter                                  *
 *                                                                            *
 ******************************************************************************/
static void	lld_filter_init(lld_filter_t *filter)
{
	zbx_vector_ptr_create(&filter->conditions);
	filter->expression = NULL;
	filter->evaltype = CONDITION_EVAL_TYPE_AND_OR;
}

/******************************************************************************
 *                                                                            *
 * Function: lld_filter_clean                                                 *
 *                                                                            *
 * Purpose: releases resources allocated by lld filter                        *
 *                                                                            *
 * Parameters: filter  - [IN] the lld filter                                  *
 *                                                                            *
 ******************************************************************************/
static void	lld_filter_clean(lld_filter_t *filter)
{
	zbx_free(filter->expression);
	lld_conditions_free(&filter->conditions);
}

/******************************************************************************
 *                                                                            *
 * Function: lld_filter_load                                                  *
 *                                                                            *
 * Purpose: loads lld filter data                                             *
 *                                                                            *
 * Parameters: filter     - [IN] the lld filter                               *
 *             lld_ruleid - [IN] the lld rule id                              *
 *             error      - [OUT] the error description                       *
 *                                                                            *
 ******************************************************************************/
static int	lld_filter_load(lld_filter_t *filter, zbx_uint64_t lld_ruleid, char **error)
{
	DB_RESULT	result;
	DB_ROW		row;
	lld_condition_t	*condition;
	DC_ITEM		item;
	int		errcode, ret = SUCCEED;

	DCconfig_get_items_by_itemids(&item, &lld_ruleid, &errcode, 1);

	if (SUCCEED != errcode)
	{
		*error = zbx_dsprintf(*error, "Invalid discovery rule ID [" ZBX_FS_UI64 "].",
				lld_ruleid);
		ret = FAIL;
		goto out;
	}

	result = DBselect(
			"select item_conditionid,macro,value,operator"
			" from item_condition"
			" where itemid=" ZBX_FS_UI64,
			lld_ruleid);

	while (NULL != (row = DBfetch(result)))
	{
		condition = (lld_condition_t *)zbx_malloc(NULL, sizeof(lld_condition_t));
		ZBX_STR2UINT64(condition->id, row[0]);
		condition->macro = zbx_strdup(NULL, row[1]);
		condition->regexp = zbx_strdup(NULL, row[2]);
		condition->op = (unsigned char)atoi(row[3]);

		zbx_vector_ptr_create(&condition->regexps);

		zbx_vector_ptr_append(&filter->conditions, condition);

		if ('@' == *condition->regexp)
		{
			DCget_expressions_by_name(&condition->regexps, condition->regexp + 1);

			if (0 == condition->regexps.values_num)
			{
				*error = zbx_dsprintf(*error, "Global regular expression \"%s\" does not exist.",
						condition->regexp + 1);
				ret = FAIL;
				break;
			}
		}
		else
		{
			substitute_simple_macros(NULL, NULL, NULL, NULL, NULL, NULL, &item, NULL, NULL,
					&condition->regexp, MACRO_TYPE_LLD_FILTER, NULL, 0);
		}
	}
	DBfree_result(result);

	if (SUCCEED != ret)
		lld_conditions_free(&filter->conditions);
	else if (CONDITION_EVAL_TYPE_AND_OR == filter->evaltype)
		zbx_vector_ptr_sort(&filter->conditions, lld_condition_compare_by_macro);
out:
	DCconfig_clean_items(&item, &errcode, 1);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_lld_macro_value_by_name                                      *
 *                                                                            *
 * Purpose: get value of LLD macro using json path if available or by         *
 *          searching for such key in key value pairs of array entry          *
 *                                                                            *
 * Parameters: jp_row          - [IN] the lld data row                        *
 *             lld_macro_paths - [IN] use json path to extract from jp_row    *
 *             macro           - [IN] LLD macro                               *
 *             value           - [OUT] value extracted from jp_row            *
 *             value_alloc     - [OUT] allocated memory size for value        *
 *                                                                            *
 ******************************************************************************/
int	zbx_lld_macro_value_by_name(const struct zbx_json_parse *jp_row, const zbx_vector_ptr_t *lld_macro_paths,
		const char *macro, char **value, size_t *value_alloc)
{
	zbx_lld_macro_path_t	lld_macro_path_local, *lld_macro_path;
	int			index;
	struct zbx_json_parse	jp_out;
	int			ret;

	lld_macro_path_local.lld_macro = (char *)macro;

	if (FAIL != (index = zbx_vector_ptr_bsearch(lld_macro_paths, &lld_macro_path_local, lld_macro_paths_compare)))
	{
		lld_macro_path = (zbx_lld_macro_path_t *)lld_macro_paths->values[index];

		if (FAIL != (ret = zbx_json_path_open(jp_row, lld_macro_path->path, &jp_out)))
			zbx_json_value_dyn(&jp_out, value, value_alloc);
	}
	else
		ret = zbx_json_value_by_name_dyn(jp_row, macro, value, value_alloc);

	return ret;
}

static int	filter_condition_match(const struct zbx_json_parse *jp_row, const zbx_vector_ptr_t *lld_macro_paths,
		const lld_condition_t *condition)
{
	char	*value = NULL;
	size_t	value_alloc = 0;
	int	ret;

	if (SUCCEED == (ret = zbx_lld_macro_value_by_name(jp_row, lld_macro_paths, condition->macro, &value,
			&value_alloc)))
	{
		switch (regexp_match_ex(&condition->regexps, value, condition->regexp, ZBX_CASE_SENSITIVE))
		{
			case ZBX_REGEXP_MATCH:
				ret = (CONDITION_OPERATOR_REGEXP == condition->op ? SUCCEED : FAIL);
				break;
			case ZBX_REGEXP_NO_MATCH:
				ret = (CONDITION_OPERATOR_NOT_REGEXP == condition->op ? SUCCEED : FAIL);
				break;
			default:
				ret = FAIL;
		}
	}

	zbx_free(value);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: filter_evaluate_and_or                                           *
 *                                                                            *
 * Purpose: check if the lld data passes filter evaluation by and/or rule     *
 *                                                                            *
 * Parameters: filter          - [IN] the lld filter                          *
 *             jp_row          - [IN] the lld data row                        *
 *             lld_macro_paths - [IN] use json path to extract from jp_row    *
 *                                                                            *
 * Return value: SUCCEED - the lld data passed filter evaluation              *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	filter_evaluate_and_or(const lld_filter_t *filter, const struct zbx_json_parse *jp_row,
		const zbx_vector_ptr_t *lld_macro_paths)
{
	const char	*__function_name = "filter_evaluate_and_or";

	int		i, ret = SUCCEED, rc = SUCCEED;
	char		*lastmacro = NULL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	for (i = 0; i < filter->conditions.values_num; i++)
	{
		const lld_condition_t	*condition = (lld_condition_t *)filter->conditions.values[i];

		rc = filter_condition_match(jp_row, lld_macro_paths, condition);
		/* check if a new condition group has started */
		if (NULL == lastmacro || 0 != strcmp(lastmacro, condition->macro))
		{
			/* if any of condition groups are false the evaluation returns false */
			if (FAIL == ret)
				break;

			ret = rc;
		}
		else
		{
			if (SUCCEED == rc)
				ret = rc;
		}

		lastmacro = condition->macro;
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: filter_evaluate_and                                              *
 *                                                                            *
 * Purpose: check if the lld data passes filter evaluation by and rule        *
 *                                                                            *
 * Parameters: filter          - [IN] the lld filter                          *
 *             jp_row          - [IN] the lld data row                        *
 *             lld_macro_paths - [IN] use json path to extract from jp_row    *
 *                                                                            *
 * Return value: SUCCEED - the lld data passed filter evaluation              *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	filter_evaluate_and(const lld_filter_t *filter, const struct zbx_json_parse *jp_row,
		const zbx_vector_ptr_t *lld_macro_paths)
{
	const char	*__function_name = "filter_evaluate_and";

	int		i, ret = SUCCEED;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	for (i = 0; i < filter->conditions.values_num; i++)
	{
		/* if any of conditions are false the evaluation returns false */
		if (SUCCEED != (ret = filter_condition_match(jp_row, lld_macro_paths,
				(lld_condition_t *)filter->conditions.values[i])))
		{
			break;
		}
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: filter_evaluate_or                                               *
 *                                                                            *
 * Purpose: check if the lld data passes filter evaluation by or rule         *
 *                                                                            *
 * Parameters: filter          - [IN] the lld filter                          *
 *             jp_row          - [IN] the lld data row                        *
 *             lld_macro_paths - [IN] use json path to extract from jp_row    *
 *                                                                            *
 * Return value: SUCCEED - the lld data passed filter evaluation              *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	filter_evaluate_or(const lld_filter_t *filter, const struct zbx_json_parse *jp_row,
		const zbx_vector_ptr_t *lld_macro_paths)
{
	const char	*__function_name = "filter_evaluate_or";

	int		i, ret = SUCCEED;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	for (i = 0; i < filter->conditions.values_num; i++)
	{
		/* if any of conditions are true the evaluation returns true */
		if (SUCCEED == (ret = filter_condition_match(jp_row, lld_macro_paths,
				(lld_condition_t *)filter->conditions.values[i])))
		{
			break;
		}
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: filter_evaluate_expression                                       *
 *                                                                            *
 * Purpose: check if the lld data passes filter evaluation by custom          *
 *          expression                                                        *
 *                                                                            *
 * Parameters: filter          - [IN] the lld filter                          *
 *             jp_row          - [IN] the lld data row                        *
 *             lld_macro_paths - [IN] use json path to extract from jp_row    *
 *                                                                            *
 * Return value: SUCCEED - the lld data passed filter evaluation              *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 * Comments: 1) replace {item_condition} references with action condition     *
 *              evaluation results (1 or 0)                                   *
 *           2) call evaluate() to calculate the final result                 *
 *                                                                            *
 ******************************************************************************/
static int	filter_evaluate_expression(const lld_filter_t *filter, const struct zbx_json_parse *jp_row,
		const zbx_vector_ptr_t *lld_macro_paths)
{
	const char	*__function_name = "filter_evaluate_expression";

	int		i, ret = FAIL, id_len;
	char		*expression, id[ZBX_MAX_UINT64_LEN + 2], *p, error[256];
	double		result;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() expression:%s", __function_name, filter->expression);

	expression = zbx_strdup(NULL, filter->expression);

	for (i = 0; i < filter->conditions.values_num; i++)
	{
		const lld_condition_t	*condition = (lld_condition_t *)filter->conditions.values[i];

		ret = filter_condition_match(jp_row, lld_macro_paths, condition);

		zbx_snprintf(id, sizeof(id), "{" ZBX_FS_UI64 "}", condition->id);

		id_len = strlen(id);
		p = expression;

		while (NULL != (p = strstr(p, id)))
		{
			*p = (SUCCEED == ret ? '1' : '0');
			memset(p + 1, ' ', id_len - 1);
			p += id_len;
		}
	}

	if (SUCCEED == evaluate(&result, expression, error, sizeof(error), NULL))
		ret = (SUCCEED != zbx_double_compare(result, 0) ? SUCCEED : FAIL);

	zbx_free(expression);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: filter_evaluate                                                  *
 *                                                                            *
 * Purpose: check if the lld data passes filter evaluation                    *
 *                                                                            *
 * Parameters: filter          - [IN] the lld filter                          *
 *             jp_row          - [IN] the lld data row                        *
 *             lld_macro_paths - [IN] use json path to extract from jp_row    *
 *                                                                            *
 * Return value: SUCCEED - the lld data passed filter evaluation              *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	filter_evaluate(const lld_filter_t *filter, const struct zbx_json_parse *jp_row,
		const zbx_vector_ptr_t *lld_macro_paths)
{
	switch (filter->evaltype)
	{
		case CONDITION_EVAL_TYPE_AND_OR:
			return filter_evaluate_and_or(filter, jp_row, lld_macro_paths);
		case CONDITION_EVAL_TYPE_AND:
			return filter_evaluate_and(filter, jp_row, lld_macro_paths);
		case CONDITION_EVAL_TYPE_OR:
			return filter_evaluate_or(filter, jp_row, lld_macro_paths);
		case CONDITION_EVAL_TYPE_EXPRESSION:
			return filter_evaluate_expression(filter, jp_row, lld_macro_paths);
	}

	return FAIL;
}

/******************************************************************************
 *                                                                            *
 * Function: lld_check_received_data_for_filter                               *
 *                                                                            *
 * Purpose: Check if the LLD data contains a values for macros used in filter.*
 *          Create an informative warning for every macro that has not        *
 *          received any value.                                               *
 *                                                                            *
 * Parameters: filter          - [IN] the lld filter                          *
 *             jp_row          - [IN] the lld data row                        *
 *             lld_macro_paths - [IN] use json path to extract from jp_row    *
 *             info            - [OUT] the warning description                *
 *                                                                            *
 ******************************************************************************/
static void	lld_check_received_data_for_filter(lld_filter_t *filter, const struct zbx_json_parse *jp_row,
		const zbx_vector_ptr_t *lld_macro_paths, char **info)
{
	int			i, index;
	zbx_lld_macro_path_t	lld_macro_path_local, *lld_macro_path;
	struct zbx_json_parse	jp_out;

	for (i = 0; i < filter->conditions.values_num; i++)
	{
		const lld_condition_t	*condition = (lld_condition_t *)filter->conditions.values[i];

		lld_macro_path_local.lld_macro = condition->macro;

		if (FAIL != (index = zbx_vector_ptr_bsearch(lld_macro_paths, &lld_macro_path_local,
				lld_macro_paths_compare)))
		{
			lld_macro_path = (zbx_lld_macro_path_t *)lld_macro_paths->values[index];

			if (FAIL == zbx_json_path_open(jp_row, lld_macro_path->path, &jp_out))
			{
				*info = zbx_strdcatf(*info,
						"Cannot accurately apply filter: no value received for macro \"%s\""
						" json path '%s'.\n", lld_macro_path->lld_macro, lld_macro_path->path);
			}

			continue;
		}

		if (NULL == zbx_json_pair_by_name(jp_row, condition->macro))
		{
			*info = zbx_strdcatf(*info,
					"Cannot accurately apply filter: no value received for macro \"%s\".\n",
					condition->macro);
		}
	}
}

static int	lld_rows_get(const char *value, lld_filter_t *filter, zbx_vector_ptr_t *lld_rows,
		const zbx_vector_ptr_t *lld_macro_paths, char **info, char **error)
{
	const char		*__function_name = "lld_rows_get";

	struct zbx_json_parse	jp, jp_array, jp_row;
	const char		*p;
	zbx_lld_row_t		*lld_row;
	int			ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	if (SUCCEED != zbx_json_open(value, &jp))
	{
		*error = zbx_strdup(*error, "Value should be a JSON array.");
		goto out;
	}

	if ('[' == *jp.start)
	{
		jp_array = jp;
	}
	else if (SUCCEED != zbx_json_brackets_by_name(&jp, ZBX_PROTO_TAG_DATA, &jp_array))	/* deprecated */
	{
		*error = zbx_dsprintf(*error, "Cannot find the \"%s\" array in the received JSON object.",
				ZBX_PROTO_TAG_DATA);
		goto out;
	}

	p = NULL;
	while (NULL != (p = zbx_json_next(&jp_array, p)))
	{
		if (FAIL == zbx_json_brackets_open(p, &jp_row))
			continue;

		lld_check_received_data_for_filter(filter, &jp_row, lld_macro_paths, info);

		if (SUCCEED != filter_evaluate(filter, &jp_row, lld_macro_paths))
			continue;

		lld_row = (zbx_lld_row_t *)zbx_malloc(NULL, sizeof(zbx_lld_row_t));
		lld_row->jp_row = jp_row;
		zbx_vector_ptr_create(&lld_row->item_links);

		zbx_vector_ptr_append(lld_rows, lld_row);
	}

	ret = SUCCEED;
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

static void	lld_item_link_free(zbx_lld_item_link_t *item_link)
{
	zbx_free(item_link);
}

static void	lld_row_free(zbx_lld_row_t *lld_row)
{
	zbx_vector_ptr_clear_ext(&lld_row->item_links, (zbx_clean_func_t)lld_item_link_free);
	zbx_vector_ptr_destroy(&lld_row->item_links);
	zbx_free(lld_row);
}

/******************************************************************************
 *                                                                            *
 * Function: lld_process_discovery_rule                                       *
 *                                                                            *
 * Purpose: add or update items, triggers and graphs for discovery item       *
 *                                                                            *
 * Parameters: lld_ruleid - [IN] discovery item identificator from database   *
 *             value      - [IN] received value from agent                    *
 *                                                                            *
 ******************************************************************************/
void	lld_process_discovery_rule(zbx_uint64_t lld_ruleid, const char *value, const zbx_timespec_t *ts)
{
	const char		*__function_name = "lld_process_discovery_rule";

	DB_RESULT		result;
	DB_ROW			row;
	zbx_uint64_t		hostid;
	char			*discovery_key = NULL, *error = NULL, *db_error = NULL, *error_esc, *info = NULL;
	unsigned char		state;
	int			lifetime;
	zbx_vector_ptr_t	lld_rows, lld_macro_paths;
	char			*sql = NULL;
	size_t			sql_alloc = 128, sql_offset = 0;
	const char		*sql_start = "update items set ", *sql_continue = ",";
	lld_filter_t		filter;
	time_t			now;
	zbx_item_diff_t		lld_rule_diff = {.itemid = lld_ruleid, .flags = ZBX_FLAGS_ITEM_DIFF_UNSET};

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() itemid:" ZBX_FS_UI64, __function_name, lld_ruleid);

	if (FAIL == DCconfig_lock_lld_rule(lld_ruleid))
	{
		zabbix_log(LOG_LEVEL_WARNING, "cannot process discovery rule \"%s\": another value is being processed",
				zbx_host_key_string(lld_ruleid));
		goto out;
	}

	zbx_vector_ptr_create(&lld_rows);
	zbx_vector_ptr_create(&lld_macro_paths);

	lld_filter_init(&filter);

	sql = (char *)zbx_malloc(sql, sql_alloc);

	result = DBselect(
			"select hostid,key_,state,evaltype,formula,error,lifetime"
			" from items"
			" where itemid=" ZBX_FS_UI64,
			lld_ruleid);

	if (NULL != (row = DBfetch(result)))
	{
		char	*lifetime_str;

		ZBX_STR2UINT64(hostid, row[0]);
		discovery_key = zbx_strdup(discovery_key, row[1]);
		state = (unsigned char)atoi(row[2]);
		filter.evaltype = atoi(row[3]);
		filter.expression = zbx_strdup(NULL, row[4]);
		db_error = zbx_strdup(db_error, row[5]);

		lifetime_str = zbx_strdup(NULL, row[6]);
		substitute_simple_macros(NULL, NULL, NULL, NULL, &hostid, NULL, NULL, NULL, NULL,
				&lifetime_str, MACRO_TYPE_COMMON, NULL, 0);

		if (SUCCEED != is_time_suffix(lifetime_str, &lifetime, ZBX_LENGTH_UNLIMITED))
		{
			zabbix_log(LOG_LEVEL_WARNING, "cannot process lost resources for the discovery rule \"%s:%s\":"
					" \"%s\" is not a valid value",
					zbx_host_string(hostid), discovery_key, lifetime_str);
			lifetime = 25 * SEC_PER_YEAR;	/* max value for the field */
		}

		zbx_free(lifetime_str);
	}
	DBfree_result(result);

	if (NULL == row)
	{
		zabbix_log(LOG_LEVEL_WARNING, "invalid discovery rule ID [" ZBX_FS_UI64 "]", lld_ruleid);
		goto clean;
	}

	if (SUCCEED != lld_filter_load(&filter, lld_ruleid, &error))
		goto error;

	if (SUCCEED != lld_macro_paths_get(lld_ruleid, &lld_macro_paths, &error))
		goto error;

	if (SUCCEED != lld_rows_get(value, &filter, &lld_rows, &lld_macro_paths, &info, &error))
		goto error;

	error = zbx_strdup(error, "");

	now = time(NULL);

	if (SUCCEED != lld_update_items(hostid, lld_ruleid, &lld_rows, &lld_macro_paths, &error, lifetime, now))
	{
		zabbix_log(LOG_LEVEL_DEBUG, "cannot update/add items because parent host was removed while"
				" processing lld rule");
		goto clean;
	}

	lld_item_links_sort(&lld_rows);

	if (SUCCEED != lld_update_triggers(hostid, lld_ruleid, &lld_rows, &lld_macro_paths, &error))
	{
		zabbix_log(LOG_LEVEL_DEBUG, "cannot update/add triggers because parent host was removed while"
				" processing lld rule");
		goto clean;
	}

	if (SUCCEED != lld_update_graphs(hostid, lld_ruleid, &lld_rows, &lld_macro_paths, &error))
	{
		zabbix_log(LOG_LEVEL_DEBUG, "cannot update/add graphs because parent host was removed while"
				" processing lld rule");
		goto clean;
	}

	lld_update_hosts(lld_ruleid, &lld_rows, &lld_macro_paths, &error, lifetime, now);

	if (ITEM_STATE_NOTSUPPORTED == state)
	{
		zabbix_log(LOG_LEVEL_WARNING, "discovery rule \"%s\" became supported", zbx_host_key_string(lld_ruleid));

		zbx_add_event(EVENT_SOURCE_INTERNAL, EVENT_OBJECT_LLDRULE, lld_ruleid, ts, ITEM_STATE_NORMAL,
				NULL, NULL, NULL, 0, 0, NULL, 0, NULL, 0, NULL);
		zbx_process_events(NULL, NULL);
		zbx_clean_events();

		zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset, "%sstate=%d", sql_start, ITEM_STATE_NORMAL);
		sql_start = sql_continue;

		lld_rule_diff.state = ITEM_STATE_NORMAL;
		lld_rule_diff.flags |= ZBX_FLAGS_ITEM_DIFF_UPDATE_STATE;
	}

	/* add informative warning to the error message about lack of data for macros used in filter */
	if (NULL != info)
		error = zbx_strdcat(error, info);
error:
	if (NULL != error && 0 != strcmp(error, db_error))
	{
		error_esc = DBdyn_escape_field("items", "error", error);

		zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset, "%serror='%s'", sql_start, error_esc);
		sql_start = sql_continue;

		zbx_free(error_esc);

		lld_rule_diff.error = error;
		lld_rule_diff.flags |= ZBX_FLAGS_ITEM_DIFF_UPDATE_ERROR;
	}

	if (sql_start == sql_continue)
	{
		zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset, " where itemid=" ZBX_FS_UI64, lld_ruleid);
		DBexecute("%s", sql);
	}

	if (ZBX_FLAGS_ITEM_DIFF_UNSET != lld_rule_diff.flags)
	{
		zbx_vector_ptr_t	diffs;

		zbx_vector_ptr_create(&diffs);
		zbx_vector_ptr_append(&diffs, &lld_rule_diff);
		DCconfig_items_apply_changes(&diffs);
		zbx_vector_ptr_destroy(&diffs);
	}
clean:
	DCconfig_unlock_lld_rule(lld_ruleid);

	zbx_free(info);
	zbx_free(error);
	zbx_free(db_error);
	zbx_free(discovery_key);
	zbx_free(sql);

	lld_filter_clean(&filter);

	zbx_vector_ptr_clear_ext(&lld_rows, (zbx_clean_func_t)lld_row_free);
	zbx_vector_ptr_destroy(&lld_rows);
	zbx_vector_ptr_clear_ext(&lld_macro_paths, (zbx_clean_func_t)lld_macro_path_free);
	zbx_vector_ptr_destroy(&lld_macro_paths);
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

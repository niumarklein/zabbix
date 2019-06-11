<?php
/*
** Zabbix
** Copyright (C) 2001-2019 Zabbix SIA
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


/**
 * Host info widget form
 */
class CWidgetFormHostsAvail extends CWidgetForm {

	public function __construct($data) {
		parent::__construct($data, WIDGET_HOSTS_AVAIL);

		// Host groups
		$field_groups = new CWidgetFieldGroup('groupids', _('Host groups'));

		if (array_key_exists('groupids', $this->data)) {
			$field_groups->setValue($this->data['groupids']);
		}
		$this->fields[$field_groups->getName()] = $field_groups;

		// Hosts info style
		$field_style = (new CWidgetFieldRadioButtonList('layout', _('Layout'), [
			STYLE_HORIZONTAL => _('Horizontal'),
			STYLE_VERTICAL => _('Vertical')
		]))
			->setDefault(STYLE_HORIZONTAL)
			->setModern(true);

		if (array_key_exists('layout', $this->data)) {
			$field_style->setValue($this->data['layout']);
		}

		$this->fields[$field_style->getName()] = $field_style;

		// Show hosts in maintenance
		$field_style = (new CWidgetFieldCheckBox('maintenance', _('Show hosts in maintenance')))
			->setDefault(HOST_MAINTENANCE_STATUS_OFF);

		if (array_key_exists('maintenance', $this->data)) {
			$field_style->setValue($this->data['maintenance']);
		}

		$this->fields[$field_style->getName()] = $field_style;
	}
}

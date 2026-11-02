'use strict';
'require view';

return view.extend({
	handleSaveApply: null,
	handleSave: null,
	handleReset: null,

	render: function() {
		return E('iframe', {
			src: '/traffic/',
			title: _('Traffic'),
			style: 'width:100%;height:85vh;border:0'
		});
	}
});

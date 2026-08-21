// Panel registry — plugins call Rdbg.registerPanel({id, title, ...hooks}).
var Rdbg = window.Rdbg || {};
Rdbg.panels = Rdbg.panels || {};
Rdbg.order = Rdbg.order || [];

Rdbg.registerPanel = function(spec) {
  if (!spec || !spec.id) return;
  if (!this.panels[spec.id]) this.order.push(spec.id);
  this.panels[spec.id] = spec;
};

Rdbg.get = function(id) {
  return this.panels[id] || null;
};

Rdbg.list = function() {
  var out = [];
  for (var i = 0; i < this.order.length; i++) {
    var spec = this.panels[this.order[i]];
    if (spec) out.push(spec);
  }
  return out;
};

window.Rdbg = Rdbg;

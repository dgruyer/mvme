##################################################
Changelog
##################################################

0.9.5
-------

.. note::
  Analysis files created by this version can not be opened by prior versions
  because the file format has changed.

This version contains major enhancements to the analysis user interface and
handling of analysis objects.

* It is now possible to export an object selection to a library file and import
  objects from library files.

* Directory objects have been added which, in addition to the previously
  existing userlevels, allow to further structure an analysis.

  Directories can contain operators, data sinks (histograms, rate monitors,
  etc.) and  other directories.

* Objects can now be moved between userlevels and directories using drag and
  drop.

* A copy/paste mechanism has been implemented which allows creating a copy of a
  selection of objects.

  If internally connected objects are copied and then pasted the connections
  will be restored on the copies.

Other fixes and changes:

* New feature: dynamic resolution reduction for 1D and 2D histograms.

  Axis display resolutions can now be adjusted via sliders in the user
  interface without having to change the physical resolution of the underlying
  histogram.

* Improved hostname lookups for the SIS3153 VME controller under Windows. The
  result is now up-to-date without requiring a restart of mvme.

* Add libpng to the linux binary package. This fixes a shared library version
  conflict under Ubuntu 18.04.

* SIS3153: OUT2 is now active during execution of the main readout stack.
  Unchanged: OUT1 is active while in autonomous DAQ mode.

* The Rate Monitor can now take multiple inputs, each of which can be an array
  or a single parameter.

  Also implemented a combined view showing all rates of a Rate Monitor in a
  single plot.

* Add new VM-USB specific vme script commands: ``vmusb_write_reg`` and
  ``vmusb_read_reg`` which allow setting up the VM-USB NIM outputs, the
  internal scalers and delay and gate generators.

  Refer to the VM-USB manual for details about these registers.

0.9.4.1
-------
* Fix expression operator GUI not properly loading indexed parameter
  connections

* Split Histo1D info box into global and gauss specific statistics. Fixes to
  gauss related calculations.

0.9.4
-------
* New: :ref:`Analysis Expression Operator<analysis-ExpressionOperator>`

  This is an operator that allows user-defined scripts to be executed for each readout
  event. Internally `exprtk`_ is used to compile and evaluate expressions.

* New: :ref:`Analysis Export Sink<analysis-ExportSink>`

  Allows exporting of analysis parameter arrays to binary files. Full and sparse data
  export formats and optional zlib compression are available.

  Source code showing how to read and process the exported data and generate ROOT
  histograms can be generated.

* New: :ref:`Analysis Rate Monitor<analysis-RateMonitorSink>`

  Allows to monitor and plot analysis data flow rates and rates calculated from successive
  counter values (e.g. timestamp differences).

* Moved the MultiEvent Processing option and the MultiEvent Module Header Filters from the
  VME side to the analysis side. This is more logical and allows changing the option when
  doing a replay.

* General fixes and improvements to the SIS3153 readout code.

* New: JSON-RPC interface using TCP as the transport mechanism.

  Allows to start/stop DAQ runs and to request status information.


0.9.3
-------

* Support for the Struck SIS3153 VME Controller using an ethernet connection
* Analysis:

  * Performance improvments
  * Better statistics
  * Can now single step through events to ease debugging
  * Add additional analysis aggregate operations: min, max, mean, sigma in x
    and y
  * Save/load of complete analysis sessions: Histogram contents are saved to
    disk and can be loaded at a later time. No new replay of the data is
    neccessary.
  * New: rate monitoring using rates generated from readout data or flow rates
    through the analysis.

* Improved mesytec vme module templates. Also added templates for the new VMMR
  module.
* More options on how the output listfile names are generated.
* Various bugfixes and improvements

0.9.2
-------

* New experimental feature: multi event readout support to achieve higher data
  rates.
* DataFilter (Extractor) behaviour change: Extraction masks do not need to be
  consecutive anymore. Instead a "bit gather" step is performed to group the
  extracted bits together and the end of the filter step.
* UI: Keep/Clear histo data on new run is now settable via radio buttons.
* VMUSB: Activate output NIM O2 while DAQ mode is active. Use the top yellow
  LED to signal "USB InFIFO Full".
* Analysis performance improvements.
* Major updates to the VME templates for mesytec modules.

0.9.1
-------

* Record a timetick every second. Timeticks are stored as sections in the
  listfile and are passed to the analyis during DAQ and replay.
* Add option to keep histo data across runs/replays
* Fixes to histograms with axis unit values >= 2^31
* Always use ZIP format for listfiles

.. _exprtk: http://www.partow.net/programming/exprtk/index.html
